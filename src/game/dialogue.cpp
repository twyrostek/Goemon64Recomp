#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#include "recomp.h"
#include "goemon_dialogue.h"
#include "goemon_support.h"

namespace {
    using goemon64::dialogue::Segment;
    using goemon64::dialogue::Token;

    std::unordered_map<uint64_t, std::shared_ptr<Segment>> db;
    std::once_flag db_loaded;
    std::mutex current_mutex;
    std::shared_ptr<const goemon64::dialogue::View> current_view_ptr;
    // Segments printed into the window since its glyph slots were last cleared.
    struct Entry {
        std::shared_ptr<const Segment> segment;
        uint32_t base_slot;
        std::vector<int> glyph_ordinal; // per code: printed glyph number or -1
    };
    std::vector<Entry> entries;
    uint32_t active_window = 0;
    std::atomic<int64_t> window_tick_ms{ 0 };

    int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    uint64_t hash_words(const std::vector<uint16_t>& words) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint16_t w : words) {
            for (uint8_t b : { uint8_t(w >> 8), uint8_t(w & 0xFF) }) {
                h = (h ^ b) * 0x100000001b3ull;
            }
        }
        return h;
    }

    void load_db() {
        std::ifstream f(goemon64::get_asset_path("dialogue_db.tsv"));
        if (!f) {
            std::printf("[dlg] dialogue_db.tsv not found, translations disabled\n");
            return;
        }
        std::string line;
        std::shared_ptr<Segment> seg;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::vector<std::string> cols;
            std::stringstream ss(line);
            std::string col;
            while (std::getline(ss, col, '\t')) {
                cols.push_back(col);
            }
            if (cols[0] == "S" && cols.size() >= 3) {
                seg = std::make_shared<Segment>();
                seg->code_count = (uint32_t)std::stoul(cols[2]);
                db[std::stoull(cols[1], nullptr, 16)] = seg;
            } else if (cols[0] == "T" && seg && cols.size() >= 5) {
                Token t;
                t.code_start = (uint32_t)std::stoul(cols[1]);
                t.code_len = (uint32_t)std::stoul(cols[2]);
                t.surface = cols[3];
                t.reading = cols[4];
                t.gloss = cols.size() > 5 ? cols[5] : "";
                t.note = cols.size() > 6 ? cols[6] : "";
                seg->tokens.push_back(std::move(t));
            }
        }
        std::printf("[dlg] loaded %zu dialogue segments\n", db.size());
    }
}

std::shared_ptr<const goemon64::dialogue::View> goemon64::dialogue::current_view() {
    std::lock_guard lock(current_mutex);
    return current_view_ptr;
}

bool goemon64::dialogue::is_active() {
    return now_ms() - window_tick_ms.load() < 200;
}

// Rebuilds the on-screen rectangles of every segment in the window from the live glyph slots.
static void rebuild_view(uint8_t* rdram) {
    if (entries.empty()) {
        std::lock_guard lock(current_mutex);
        current_view_ptr = nullptr;
        return;
    }

    gpr w = (gpr)(int32_t)active_window;
    auto slot_active = [&](int i) { return (int16_t)MEM_H(0x198 + i * 0xB8 + 4, w) != 0; };
    const float win_x = (int16_t)MEM_H(0x32, w);
    const float win_y = (int16_t)MEM_H(0x34, w);

    auto view = std::make_shared<goemon64::dialogue::View>();
    for (const auto& e : entries) {
        view->segments.push_back(e.segment);
        for (const auto& t : e.segment->tokens) {
            goemon64::dialogue::ViewToken vt{ &t, {} };
            for (uint32_t c = t.code_start; c < t.code_start + t.code_len && c < e.glyph_ordinal.size(); c++) {
                if (e.glyph_ordinal[c] < 0) {
                    continue;
                }
                int slot = (int)e.base_slot + e.glyph_ordinal[c];
                if (slot >= 60 || !slot_active(slot)) {
                    continue;
                }
                int32_t base = 0x198 + slot * 0xB8;
                // Slot coordinates are the glyph centre inside the window; glyphs are 13x16.
                goemon64::dialogue::Rect r{ win_x + (int16_t)MEM_H(base + 8, w) - 6.5f, win_y + (int16_t)MEM_H(base + 0xA, w) - 8.0f, 13.0f, 16.0f };
                if (!vt.rects.empty() && vt.rects.back().y == r.y && std::abs(vt.rects.back().x + vt.rects.back().w - r.x) < 1.0f) {
                    vt.rects.back().w += r.w;
                } else {
                    vt.rects.push_back(r);
                }
            }
            view->tokens.push_back(std::move(vt));
        }
    }

    std::lock_guard lock(current_mutex);
    current_view_ptr = view;
}

// Called from a hook at the entry of the JP text printer (a0 = window id, a1 = pointer to the next s16 glyph code).
extern "C" void goemon_dialogue_print_hook(uint8_t* rdram, recomp_context* ctx) {
    static uint32_t last_start = 0;
    uint32_t ptr = (uint32_t)ctx->r5;
    if (ptr < 0x80000000u || ptr >= 0x80800000u) {
        return;
    }

    auto word = [&](uint32_t addr) -> uint16_t { return (uint16_t)MEM_HU(0, (gpr)(int32_t)addr); };

    uint32_t win = (uint32_t)MEM_W(0, (gpr)(int32_t)(0x8016D430u + (uint32_t)ctx->r4 * 4));
    if (win < 0x80000000u || win >= 0x80800000u) {
        return;
    }
    gpr w = (gpr)(int32_t)win;

    // Messages end with 0xFFFF, so the start is just after the previous terminator.
    uint32_t start = ptr;
    int back = 0;
    while (back < 1024 && word(start - 2) != 0xFFFF) {
        start -= 2;
        back++;
    }
    if (back >= 1024) {
        return;
    }

    if (start != last_start || win != active_window) {
        last_start = start;

        std::vector<uint16_t> words;
        for (uint32_t a = start; words.size() < 512; a += 2) {
            words.push_back(word(a));
            if (words.back() == 0xFFFF) {
                break;
            }
        }

        // Glyphs go into the first free slot, and a page change clears them all.
        uint32_t base_slot = 0;
        while (base_slot < 60 && (int16_t)MEM_H(0x198 + base_slot * 0xB8 + 4, w) != 0) {
            base_slot++;
        }
        if (base_slot == 0 || win != active_window) {
            entries.clear();
        }
        active_window = win;

        std::call_once(db_loaded, load_db);
        auto it = db.find(hash_words(words));
        if (it != db.end()) {
            Entry e;
            e.segment = it->second;
            e.base_slot = base_slot;
            e.glyph_ordinal.assign(words.size(), -1);
            int n = 0;
            for (size_t i = 0; i < words.size(); i++) {
                if (words[i] < 0x8000 && words[i] != 0) {
                    e.glyph_ordinal[i] = n++;
                }
            }
            entries.push_back(std::move(e));
        }
    }

    rebuild_view(rdram);
    window_tick_ms = now_ms();
    static int prints = 0;
    if (prints++ < 3) {
        std::printf("[dlg] print hook: entries=%zu\n", entries.size());
        std::fflush(stdout);
    }
}

// Called from a hook at the entry of the JP per-frame window update (a0 = window id).
extern "C" void goemon_dialogue_window_hook(uint8_t* rdram, recomp_context* ctx) {
    uint32_t win = (uint32_t)MEM_W(0, (gpr)(int32_t)(0x8016D430u + (uint32_t)ctx->r4 * 4));
    if (win == 0 || win != active_window || entries.empty()) {
        return;
    }
    int16_t state = (int16_t)MEM_H(0x30, (gpr)(int32_t)win);
    if (state < 1 || state > 6) {
        return;
    }
    rebuild_view(rdram);
    window_tick_ms = now_ms();
    static int ticks = 0;
    if (ticks++ < 3) {
        std::printf("[dlg] window hook active\n");
        std::fflush(stdout);
    }
}
