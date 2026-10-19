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
    using goemon64::dialogue::Page;
    using goemon64::dialogue::Ruby;
    using goemon64::dialogue::Segment;
    using goemon64::dialogue::Token;

    std::unordered_map<uint64_t, std::vector<std::shared_ptr<const Page>>> pages_by_first_segment;
    struct Location {
        std::shared_ptr<const Page> page;
        size_t index;
    };
    std::unordered_map<uint64_t, std::vector<Location>> segment_locations;
    std::once_flag db_loaded;
    std::mutex current_mutex;
    std::shared_ptr<const goemon64::dialogue::View> current_view_ptr;

    // Segments printed into the window since its glyph slots were last cleared.
    struct Entry {
        std::shared_ptr<const Segment> segment;
        uint32_t base_slot;
        uint32_t start_ptr;
        std::vector<int> glyph_ordinal; // per code: printed glyph number or -1
    };
    std::vector<Entry> entries;
    std::shared_ptr<const Page> current_page;
    size_t next_segment = 0;
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
        std::shared_ptr<Page> page;
        std::shared_ptr<Segment> seg;
        size_t page_count = 0;
        auto finish_page = [&]() {
            if (page && !page->segment_hashes.empty()) {
                pages_by_first_segment[page->segment_hashes.front()].push_back(page);
                for (size_t i = 0; i < page->segment_hashes.size(); i++) {
                    segment_locations[page->segment_hashes[i]].push_back({ page, i });
                }
                page_count++;
            }
            page = nullptr;
            seg = nullptr;
        };
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
            if (cols[0] == "P") {
                finish_page();
                page = std::make_shared<Page>();
            } else if (cols[0] == "L" && page) {
                page->translation = cols.size() > 1 ? cols[1] : "";
            } else if (cols[0] == "V" && page && cols.size() >= 3) {
                page->voice = cols[1];
                page->audio_key = cols[2];
            } else if (cols[0] == "S" && page && cols.size() >= 3) {
                seg = std::make_shared<Segment>();
                seg->code_count = (uint32_t)std::stoul(cols[2]);
                page->segment_hashes.push_back(std::stoull(cols[1], nullptr, 16));
                page->segments.push_back(seg);
            } else if (cols[0] == "R" && seg && cols.size() >= 4) {
                seg->ruby.push_back(Ruby{ (uint32_t)std::stoul(cols[1]), (uint32_t)std::stoul(cols[2]), cols[3] });
            } else if (cols[0] == "T" && seg && cols.size() >= 5) {
                Token t;
                t.code_start = (uint32_t)std::stoul(cols[1]);
                t.code_len = (uint32_t)std::stoul(cols[2]);
                t.surface = cols[3];
                t.reading = cols[4];
                t.gloss = cols.size() > 5 ? cols[5] : "";
                t.note = cols.size() > 6 ? cols[6] : "";
                t.audio_key = cols.size() > 7 ? cols[7] : "";
                if (cols.size() > 8) {
                    std::stringstream ps(cols[8]);
                    std::string pair;
                    while (std::getline(ps, pair, '\x1e')) {
                        const size_t sep = pair.find('\x1f');
                        if (sep != std::string::npos) {
                            t.pairs.emplace_back(pair.substr(0, sep), pair.substr(sep + 1));
                        }
                    }
                }
                seg->tokens.push_back(std::move(t));
            }
        }
        finish_page();
        std::printf("[dlg] loaded %zu dialogue pages\n", page_count);
    }
}

bool goemon64::dialogue::is_active() {
    return now_ms() - window_tick_ms.load() < 200;
}

std::shared_ptr<const goemon64::dialogue::View> goemon64::dialogue::current_view() {
    std::lock_guard lock(current_mutex);
    return current_view_ptr;
}

// Rebuilds the on-screen rectangles of every segment in the window from the live glyph slots.
static void rebuild_view(uint8_t* rdram) {
    if (entries.empty() || !current_page) {
        std::lock_guard lock(current_mutex);
        current_view_ptr = nullptr;
        return;
    }

    gpr w = (gpr)(int32_t)active_window;
    auto slot_active = [&](int i) { return (int16_t)MEM_H(0x198 + i * 0xB8 + 4, w) != 0; };
    const float win_x = (int16_t)MEM_H(0x32, w);
    const float win_y = (int16_t)MEM_H(0x34, w);

    auto view = std::make_shared<goemon64::dialogue::View>();
    view->page = current_page;
    view->box = { win_x, win_y, (float)(int16_t)MEM_H(0x68, w), (float)(int16_t)MEM_H(0x6A, w) };

    // Screen rectangles (one per text line) of a range of glyph codes in a segment.
    auto rects_for = [&](const Entry& e, uint32_t code_start, uint32_t code_len) {
        std::vector<goemon64::dialogue::Rect> rects;
        for (uint32_t c = code_start; c < code_start + code_len && c < e.glyph_ordinal.size(); c++) {
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
            if (!rects.empty() && rects.back().y == r.y && std::abs(rects.back().x + rects.back().w - r.x) < 1.0f) {
                rects.back().w += r.w;
            } else {
                rects.push_back(r);
            }
        }
        return rects;
    };

    for (const auto& e : entries) {
        view->segments.push_back(e.segment);
        for (const auto& r : e.segment->ruby) {
            view->rubies.push_back({ &r, rects_for(e, r.code_start, r.code_len) });
        }
        for (const auto& t : e.segment->tokens) {
            view->tokens.push_back({ &t, rects_for(e, t.code_start, t.code_len) });
        }
    }

    std::lock_guard lock(current_mutex);
    current_view_ptr = view;
}

// Called from a hook at the entry of the JP text printer (a0 = window id, a1 = pointer to the next s16 glyph code).
// `whole_string` is set when the game prints a complete string at once (menu options) instead of one glyph per call.
static void handle_text(uint8_t* rdram, uint32_t window_id, uint32_t ptr, bool whole_string) {
    static uint32_t segment_start = 0;
    static uint32_t segment_end = 0; // address of the 0xFFFF terminator
    if (ptr < 0x80000000u || ptr >= 0x80800000u) {
        return;
    }

    auto word = [&](uint32_t addr) -> uint16_t { return (uint16_t)MEM_HU(0, (gpr)(int32_t)addr); };

    uint32_t win = (uint32_t)MEM_W(0, (gpr)(int32_t)(0x8016D430u + window_id * 4));
    if (win < 0x80000000u || win >= 0x80800000u) {
        return;
    }
    gpr w = (gpr)(int32_t)win;

    // The first printer call for a message points at its first code, so any pointer outside the current one starts a new message.
    if (whole_string && win == active_window) {
        for (const auto& e : entries) {
            if (e.start_ptr == ptr) {
                return; // already shown; the game may redraw its menu
            }
        }
    }

    if (whole_string || ptr < segment_start || ptr > segment_end || win != active_window) {
        std::vector<uint16_t> words;
        for (uint32_t a = ptr; words.size() < 512; a += 2) {
            words.push_back(word(a));
            if (words.back() == 0xFFFF) {
                break;
            }
        }
        if (!whole_string) {
            segment_start = ptr;
            segment_end = ptr + 2 * (uint32_t)(words.size() - 1);
        }

        // Glyphs go into the first free slot, and a page change clears them all.
        uint32_t base_slot = 0;
        while (base_slot < 60 && (int16_t)MEM_H(0x198 + base_slot * 0xB8 + 4, w) != 0) {
            base_slot++;
        }
        std::call_once(db_loaded, load_db);
        const uint64_t hash = hash_words(words);

        if (base_slot == 0 || win != active_window) {
            entries.clear();
            current_page = nullptr;
            next_segment = 0;
        }
        active_window = win;

        auto push_entry = [&](const std::shared_ptr<const Segment>& segment) {
            Entry e;
            e.segment = segment;
            e.base_slot = base_slot;
            e.start_ptr = ptr;
            e.glyph_ordinal.assign(words.size(), -1);
            int n = 0;
            for (size_t k = 0; k < words.size(); k++) {
                if (words[k] < 0x8000 && words[k] != 0) {
                    e.glyph_ordinal[k] = n++;
                }
            }
            entries.push_back(std::move(e));
        };

        auto consume = [&]() {
            if (!current_page) {
                return false;
            }
            for (size_t i = next_segment; i < current_page->segment_hashes.size(); i++) {
                if (current_page->segment_hashes[i] == hash) {
                    push_entry(current_page->segments[i]);
                    next_segment = i + 1;
                    return true;
                }
            }
            return false;
        };

        if (!consume()) {
            auto first = pages_by_first_segment.find(hash);
            auto any = segment_locations.find(hash);
            if (!current_page) {
                // The window was just cleared: this message starts a page, possibly in the middle of a stored one.
                if (first != pages_by_first_segment.end()) {
                    current_page = first->second.front();
                    next_segment = 0;
                    consume();
                } else if (any != segment_locations.end()) {
                    current_page = any->second.front().page;
                    next_segment = any->second.front().index;
                    consume();
                }
            } else if (any != segment_locations.end()) {
                // Extra text printed into the same window, such as a menu option from another page; the translation stays.
                push_entry(any->second.front().page->segments[any->second.front().index]);
            }
        }
        std::printf("[dlg] bank=%d segment %08X hash=%016llX base_slot=%u page=%d entries=%zu words=", (int)MEM_W(0, (gpr)(int32_t)0x801CD018u), segment_start, (unsigned long long)hash, base_slot, current_page ? 1 : 0, entries.size());
        for (size_t k = 0; k < words.size() && k < 24; k++) {
            std::printf("%04X ", words[k]);
        }
        std::printf("\n");
        std::fflush(stdout);
    }

    rebuild_view(rdram);
    window_tick_ms = now_ms();
}

// Called from a hook at the entry of the JP text printer (a0 = window id, a1 = pointer to the next s16 glyph code).
extern "C" void goemon_dialogue_print_hook(uint8_t* rdram, recomp_context* ctx) {
    uint32_t ptr = (uint32_t)ctx->r5;
    if (ptr >= 0x80000000u && ptr < 0x80800000u) {
        handle_text(rdram, (uint32_t)ctx->r4, ptr, false);
    }
}

// Called from a hook at the entry of the JP whole-string printer used for menu options (a0 = window id, a1 = string).
extern "C" void goemon_dialogue_string_hook(uint8_t* rdram, recomp_context* ctx) {
    uint32_t ptr = (uint32_t)ctx->r5;
    if (ptr >= 0x80000000u && ptr < 0x80800000u) {
        handle_text(rdram, (uint32_t)ctx->r4, ptr, true);
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
}
