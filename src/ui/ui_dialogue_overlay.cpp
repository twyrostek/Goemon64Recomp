#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "SDL.h"
#include "RmlUi/Core.h"

#include "goemon_dialogue.h"
#include "ui_dialogue_overlay.h"

namespace {
    // RT64 shows the 320x219 visible part of the JP framebuffer inside a centred 4:3 canvas that is 251 game rows tall.
    // Measured from screenshots in the original aspect mode; the game rows start 1.9 canvas rows below the canvas top.
    constexpr float canvas_height = 251.0f;
    constexpr float canvas_width = canvas_height * 4.0f / 3.0f;
    constexpr float game_width = 320.0f;
    constexpr float canvas_pad_x = (canvas_width - game_width) * 0.5f;
    constexpr float canvas_pad_y = 1.9f;

    Rml::ElementDocument* overlay_doc = nullptr;
    const goemon64::dialogue::View* shown_view = nullptr;
    std::shared_ptr<const goemon64::dialogue::View> shown_view_ref;
    int hovered_token = -1;

    std::string escape(const std::string& s) {
        std::string out;
        for (char c : s) {
            switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            default: out += c;
            }
        }
        return out;
    }

    struct Mapping {
        float scale;
        float offset_x;
        float offset_y;
        float dp_ratio;
    };

    // Position of a game-space point in density independent pixels (the game is shown 4:3 and centred).
    float to_dp_x(const Mapping& m, float gx) { return (m.offset_x + gx * m.scale) / m.dp_ratio; }
    float to_dp_y(const Mapping& m, float gy) { return (m.offset_y + gy * m.scale) / m.dp_ratio; }

    void style_box(Rml::Element* e, const Mapping& m, float x, float y, float w, float h) {
        e->SetProperty("position", "absolute");
        e->SetProperty("left", std::to_string(to_dp_x(m, x)) + "dp");
        e->SetProperty("top", std::to_string(to_dp_y(m, y)) + "dp");
        e->SetProperty("width", std::to_string(w * m.scale / m.dp_ratio) + "dp");
        e->SetProperty("height", std::to_string(h * m.scale / m.dp_ratio) + "dp");
    }

    void rebuild(const goemon64::dialogue::View& view, const Mapping& m) {
        overlay_doc->SetInnerRML("");
        for (size_t i = 0; i < view.tokens.size(); i++) {
            const auto& vt = view.tokens[i];
            if (vt.token->reading.empty() || vt.rects.empty()) {
                continue;
            }
            // Furigana sits on the top edge of the first line the word covers.
            const auto& r = vt.rects.front();
            Rml::ElementPtr ruby = overlay_doc->CreateElement("div");
            const float h = 7.0f;
            style_box(ruby.get(), m, r.x, r.y - 4.0f, r.w, h);
            ruby->SetProperty("font-family", "LatoLatin");
            ruby->SetProperty("font-size", std::to_string(6.0f * m.scale / m.dp_ratio) + "dp");
            ruby->SetProperty("text-align", "center");
            ruby->SetProperty("white-space", "nowrap");
            ruby->SetProperty("color", "#ffffff");
            ruby->SetProperty("background-color", "#000000a0");
            ruby->SetInnerRML(escape(vt.token->reading));
            overlay_doc->AppendChild(std::move(ruby));
        }
    }

    void show_tooltip(const goemon64::dialogue::ViewToken& vt, const Mapping& m) {
        Rml::Element* old = overlay_doc->GetElementById("tooltip");
        if (old) {
            old->GetParentNode()->RemoveChild(old);
        }
        Rml::ElementPtr tip = overlay_doc->CreateElement("div");
        tip->SetId("tooltip");
        const auto& r = vt.rects.back();
        tip->SetProperty("position", "absolute");
        tip->SetProperty("left", std::to_string(to_dp_x(m, r.x)) + "dp");
        tip->SetProperty("top", std::to_string(to_dp_y(m, r.y + r.h + 8.0f)) + "dp");
        tip->SetProperty("font-family", "LatoLatin");
        tip->SetProperty("font-size", std::to_string(9.0f * m.scale / m.dp_ratio) + "dp");
        tip->SetProperty("color", "#ffffff");
        tip->SetProperty("background-color", "#101830f0");
        tip->SetProperty("padding", std::to_string(3.0f * m.scale / m.dp_ratio) + "dp");
        tip->SetProperty("border-width", "2dp");
        tip->SetProperty("border-color", "#e8c040");
        tip->SetProperty("white-space", "nowrap");

        std::string rml = "<b>" + escape(vt.token->surface) + "</b>";
        if (!vt.token->reading.empty()) {
            rml += " (" + escape(vt.token->reading) + ")";
        }
        rml += "<br/>" + escape(vt.token->gloss);
        if (!vt.token->note.empty()) {
            rml += "<br/><span style=\"color: #b0b8d0\">" + escape(vt.token->note) + "</span>";
        }
        tip->SetInnerRML(rml);
        overlay_doc->AppendChild(std::move(tip));
    }

    void hide_tooltip() {
        Rml::Element* old = overlay_doc->GetElementById("tooltip");
        if (old) {
            old->GetParentNode()->RemoveChild(old);
        }
    }
}

bool goemon64::dialogue_overlay_wanted() {
    return goemon64::dialogue::is_active() && goemon64::dialogue::current_view() != nullptr;
}

void goemon64::update_dialogue_overlay(Rml::Context* context, int fb_width, int fb_height, bool wanted) {
    if (!overlay_doc) {
        if (!wanted) {
            return;
        }
        overlay_doc = context->CreateDocument();
        overlay_doc->SetProperty("position", "absolute");
        overlay_doc->SetProperty("left", "0dp");
        overlay_doc->SetProperty("top", "0dp");
        overlay_doc->SetProperty("width", "100%");
        overlay_doc->SetProperty("height", "100%");
        overlay_doc->SetProperty("pointer-events", "none");
    }

    auto view = wanted ? goemon64::dialogue::current_view() : nullptr;
    static int last_state = -1;
    int state = wanted ? (view ? 2 : 1) : 0;
    if (state != last_state) {
        last_state = state;
        std::printf("[ovl] wanted=%d view=%d fb=%dx%d dp=%.3f\n", (int)wanted, view ? 1 : 0, fb_width, fb_height, context->GetDensityIndependentPixelRatio());
        std::fflush(stdout);
    }
    if (!view) {
        overlay_doc->Hide();
        shown_view_ref = nullptr;
        shown_view = nullptr;
        hovered_token = -1;
        return;
    }

    Mapping m;
    m.scale = std::min(fb_width / canvas_width, fb_height / canvas_height);
    m.offset_x = (fb_width - canvas_width * m.scale) * 0.5f + canvas_pad_x * m.scale;
    m.offset_y = (fb_height - canvas_height * m.scale) * 0.5f + canvas_pad_y * m.scale;
    m.dp_ratio = context->GetDensityIndependentPixelRatio();

    // Rebuild the furigana whenever the rectangles change (they are republished every frame, so compare contents).
    bool changed = !shown_view_ref || shown_view_ref->tokens.size() != view->tokens.size();
    if (!changed) {
        for (size_t i = 0; i < view->tokens.size() && !changed; i++) {
            changed = view->tokens[i].token != shown_view_ref->tokens[i].token || view->tokens[i].rects.size() != shown_view_ref->tokens[i].rects.size();
            for (size_t j = 0; j < view->tokens[i].rects.size() && !changed; j++) {
                const auto& a = view->tokens[i].rects[j];
                const auto& b = shown_view_ref->tokens[i].rects[j];
                changed = a.x != b.x || a.y != b.y || a.w != b.w;
            }
        }
    }
    if (changed) {
        rebuild(*view, m);
        hovered_token = -1;
    }
    shown_view_ref = view;

    // Hit test the mouse against the word rectangles.
    int mx = 0, my = 0;
    SDL_Window* sdl_window = SDL_GetMouseFocus();
    int hit = -1;
    if (sdl_window) {
        SDL_GetMouseState(&mx, &my);
        int win_w = 1, win_h = 1;
        SDL_GetWindowSize(sdl_window, &win_w, &win_h);
        float gx = ((mx * (float)fb_width / win_w) - m.offset_x) / m.scale;
        float gy = ((my * (float)fb_height / win_h) - m.offset_y) / m.scale;
        for (size_t i = 0; i < view->tokens.size() && hit < 0; i++) {
            for (const auto& r : view->tokens[i].rects) {
                if (gx >= r.x && gx < r.x + r.w && gy >= r.y && gy < r.y + r.h) {
                    hit = (int)i;
                    break;
                }
            }
        }
    }
    if (hit != hovered_token || changed) {
        hovered_token = hit;
        if (hit >= 0) {
            show_tooltip(view->tokens[hit], m);
        } else {
            hide_tooltip();
        }
    }

    overlay_doc->Show();
}
