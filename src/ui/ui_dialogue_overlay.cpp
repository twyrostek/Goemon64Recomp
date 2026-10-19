#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "SDL.h"
#include "RmlUi/Core.h"

#include "goemon_dialogue.h"
#include "goemon_tts.h"
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
    std::shared_ptr<const goemon64::dialogue::View> shown_view_ref;
    int hovered_token = -1;     // index of the phrase whose tooltip is shown
    bool shown_pinned = false;
    int active_pair = -1;     // aligned piece under the mouse inside a pinned tooltip
    size_t pair_count = 0;    // number of aligned pieces in the shown tooltip
    const goemon64::dialogue::Token* pinned_token = nullptr;
    bool translation_open = false;
    bool prev_left_down = false;

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

    float to_dp_x(const Mapping& m, float gx) { return (m.offset_x + gx * m.scale) / m.dp_ratio; }
    float to_dp_y(const Mapping& m, float gy) { return (m.offset_y + gy * m.scale) / m.dp_ratio; }
    float size_dp(const Mapping& m, float g) { return g * m.scale / m.dp_ratio; }

    void style_box(Rml::Element* e, const Mapping& m, float x, float y, float w, float h) {
        e->SetProperty("position", "absolute");
        e->SetProperty("left", std::to_string(to_dp_x(m, x)) + "dp");
        e->SetProperty("top", std::to_string(to_dp_y(m, y)) + "dp");
        e->SetProperty("width", std::to_string(size_dp(m, w)) + "dp");
        e->SetProperty("height", std::to_string(size_dp(m, h)) + "dp");
    }

    // The translate button sits under the bottom right corner of the dialogue box.
    goemon64::dialogue::Rect icon_rect(const goemon64::dialogue::View& view) {
        return { view.box.x + view.box.w - 20.0f, view.box.y + view.box.h + 3.0f, 20.0f, 12.0f };
    }

    // Rectangle of the "play whole box" button, left of the translate button.
    goemon64::dialogue::Rect play_rect(const goemon64::dialogue::View& view) {
        const auto ic = icon_rect(view);
        return { ic.x - 18.0f, ic.y, 16.0f, ic.h };
    }

    // True when a framebuffer pixel is inside an element's box.
    bool over_element(Rml::Element* e, float px, float py) {
        if (!e) {
            return false;
        }
        const Rml::Vector2f o = e->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f sz = e->GetBox().GetSize(Rml::BoxArea::Border);
        return px >= o.x && px < o.x + sz.x && py >= o.y && py < o.y + sz.y;
    }

    void apply_panel_visibility() {
        Rml::Element* panel = overlay_doc->GetElementById("translation");
        if (panel) {
            panel->SetProperty("display", translation_open ? "block" : "none");
        }
    }

    // Shows the full translation, with the piece that the hovered phrase stands for highlighted.
    void update_translation_text(const std::string& full, const std::string& piece) {
        Rml::Element* panel = overlay_doc->GetElementById("translation");
        if (!panel) {
            return;
        }
        size_t at = piece.empty() ? std::string::npos : full.find(piece);
        if (at == std::string::npos) {
            panel->SetInnerRML(escape(full));
            return;
        }
        panel->SetInnerRML(escape(full.substr(0, at)) + "<span style=\"background-color: #e8c040a0; color: #101830\">" + escape(piece) + "</span>" + escape(full.substr(at + piece.size())));
    }

    void rebuild(const goemon64::dialogue::View& view, const Mapping& m) {
        overlay_doc->SetInnerRML("");

        for (const auto& vr : view.rubies) {
            if (vr.rects.empty() || vr.ruby->reading.empty()) {
                continue;
            }
            // Furigana sits on the top edge of the first line the word covers.
            const auto& r = vr.rects.front();
            Rml::ElementPtr ruby = overlay_doc->CreateElement("div");
            style_box(ruby.get(), m, r.x, r.y - 4.0f, r.w, 7.0f);
            ruby->SetProperty("font-family", "LatoLatin");
            ruby->SetProperty("font-size", std::to_string(size_dp(m, 6.0f)) + "dp");
            ruby->SetProperty("text-align", "center");
            ruby->SetProperty("white-space", "nowrap");
            ruby->SetProperty("color", "#ffffff");
            ruby->SetProperty("background-color", "#000000a0");
            ruby->SetInnerRML(escape(vr.ruby->reading));
            overlay_doc->AppendChild(std::move(ruby));
        }

        if (view.page && !view.page->translation.empty()) {
            const auto ic = icon_rect(view);
            Rml::ElementPtr icon = overlay_doc->CreateElement("div");
            icon->SetId("translate_icon");
            style_box(icon.get(), m, ic.x, ic.y, ic.w, ic.h);
            icon->SetProperty("font-family", "LatoLatin");
            icon->SetProperty("font-weight", "bold");
            icon->SetProperty("font-size", std::to_string(size_dp(m, 8.0f)) + "dp");
            icon->SetProperty("line-height", std::to_string(size_dp(m, ic.h)) + "dp");
            icon->SetProperty("text-align", "center");
            icon->SetProperty("color", "#ffe060");
            icon->SetProperty("background-color", "#101830f0");
            icon->SetProperty("border-width", "2dp");
            icon->SetProperty("border-color", "#e8c040");
            icon->SetInnerRML("EN");
            overlay_doc->AppendChild(std::move(icon));

            if (goemon64::tts::available(view.page->audio_key)) {
                const auto pr = play_rect(view);
                Rml::ElementPtr play = overlay_doc->CreateElement("div");
                play->SetId("full_play");
                style_box(play.get(), m, pr.x, pr.y, pr.w, pr.h);
                play->SetProperty("font-family", "LatoLatin");
                play->SetProperty("font-size", std::to_string(size_dp(m, 8.0f)) + "dp");
                play->SetProperty("line-height", std::to_string(size_dp(m, pr.h)) + "dp");
                play->SetProperty("text-align", "center");
                play->SetProperty("color", "#ffe060");
                play->SetProperty("background-color", "#101830f0");
                play->SetProperty("border-width", "2dp");
                play->SetProperty("border-color", "#e8c040");
                play->SetInnerRML("&#9654;");
                overlay_doc->AppendChild(std::move(play));
            }

            Rml::ElementPtr panel = overlay_doc->CreateElement("div");
            panel->SetId("translation");
            panel->SetProperty("position", "absolute");
            panel->SetProperty("left", std::to_string(to_dp_x(m, view.box.x)) + "dp");
            panel->SetProperty("top", std::to_string(to_dp_y(m, ic.y + ic.h + 3.0f)) + "dp");
            panel->SetProperty("width", std::to_string(size_dp(m, view.box.w - 6.0f)) + "dp");
            panel->SetProperty("font-family", "LatoLatin");
            panel->SetProperty("font-size", std::to_string(size_dp(m, 9.0f)) + "dp");
            panel->SetProperty("color", "#ffffff");
            panel->SetProperty("background-color", "#101830f0");
            panel->SetProperty("padding", std::to_string(size_dp(m, 3.0f)) + "dp");
            panel->SetProperty("border-width", "2dp");
            panel->SetProperty("border-color", "#e8c040");
            panel->SetInnerRML(escape(view.page->translation));
            overlay_doc->AppendChild(std::move(panel));
            apply_panel_visibility();
        }
    }

    // Highlights piece `index` (and nothing else) in the shown tooltip.
    void set_active_pair(int index) {
        for (size_t i = 0; i < pair_count; i++) {
            const bool on = (int)i == index;
            for (const char* side : { "jp_", "en_" }) {
                Rml::Element* e = overlay_doc->GetElementById(side + std::to_string(i));
                if (e) {
                    e->SetProperty("background-color", on ? "#e8c040" : "#303c68");
                    e->SetProperty("color", on ? "#101830" : "#ffffff");
                }
            }
        }
        active_pair = index;
    }

    void show_tooltip(const goemon64::dialogue::ViewToken& vt, const Mapping& m, bool pinned) {
        if (vt.rects.empty()) {
            return;
        }
        Rml::Element* old = overlay_doc->GetElementById("tooltip");
        if (old) {
            old->GetParentNode()->RemoveChild(old);
        }
        Rml::ElementPtr tip = overlay_doc->CreateElement("div");
        tip->SetId("tooltip");
        const auto& r = vt.rects.back();
        tip->SetProperty("position", "absolute");
        tip->SetProperty("left", std::to_string(to_dp_x(m, r.x)) + "dp");
        tip->SetProperty("top", std::to_string(to_dp_y(m, r.y + r.h + 3.0f)) + "dp");
        tip->SetProperty("font-family", "LatoLatin");
        tip->SetProperty("font-size", std::to_string(size_dp(m, 9.0f)) + "dp");
        tip->SetProperty("color", "#ffffff");
        tip->SetProperty("background-color", "#101830f0");
        tip->SetProperty("padding", std::to_string(size_dp(m, 3.0f)) + "dp");
        tip->SetProperty("border-width", "2dp");
        tip->SetProperty("border-color", pinned ? "#ffffff" : "#e8c040");
        tip->SetProperty("white-space", "nowrap");

        // With a pinned phrase the Japanese and English text are cut into aligned pieces that highlight each other.
        const bool pieces = pinned && !vt.token->pairs.empty();
        active_pair = -1;
        pair_count = pieces ? vt.token->pairs.size() : 0;
        const std::string chip = "display: inline-block; background-color: #303c68; color: #ffffff";
        std::string jp_rml, en_rml;
        if (pieces) {
            const std::string& jp = vt.token->surface;
            size_t cursor = 0;
            for (size_t i = 0; i < vt.token->pairs.size(); i++) {
                const size_t at = jp.find(vt.token->pairs[i].first, cursor);
                if (at == std::string::npos) {
                    continue;
                }
                jp_rml += escape(jp.substr(cursor, at - cursor)) + "<span id=\"jp_" + std::to_string(i) + "\" style=\"" + chip + "\">" + escape(vt.token->pairs[i].first) + "</span>";
                cursor = at + vt.token->pairs[i].first.size();
            }
            jp_rml += escape(jp.substr(cursor));

            const std::string& en = vt.token->gloss;
            struct Place { size_t at, len, index; };
            std::vector<Place> places;
            for (size_t i = 0; i < vt.token->pairs.size(); i++) {
                const std::string& piece = vt.token->pairs[i].second;
                for (size_t from = 0; !piece.empty();) {
                    const size_t at = en.find(piece, from);
                    if (at == std::string::npos) {
                        break;
                    }
                    bool free_spot = true;
                    for (const auto& p : places) {
                        free_spot = free_spot && (at + piece.size() <= p.at || at >= p.at + p.len);
                    }
                    if (free_spot) {
                        places.push_back({ at, piece.size(), i });
                        break;
                    }
                    from = at + 1;
                }
            }
            std::sort(places.begin(), places.end(), [](const Place& a, const Place& b) { return a.at < b.at; });
            size_t en_cursor = 0;
            for (const auto& p : places) {
                en_rml += escape(en.substr(en_cursor, p.at - en_cursor)) + "<span id=\"en_" + std::to_string(p.index) + "\" style=\"" + chip + "\">" + escape(en.substr(p.at, p.len)) + "</span>";
                en_cursor = p.at + p.len;
            }
            en_rml += escape(en.substr(en_cursor));
        }

        std::string rml = "<b>" + (pieces ? jp_rml : escape(vt.token->surface)) + "</b>";
        if (!vt.token->reading.empty()) {
            rml += " (" + escape(vt.token->reading) + ")";
        }
        if (goemon64::tts::available(vt.token->audio_key)) {
            const std::string box = std::to_string(size_dp(m, 11.0f)) + "dp";
            rml += " <div id=\"tip_play\" style=\"display: inline-block; width: " + box + "; height: " + box + "; line-height: " + box +
                "; text-align: center; color: #ffe060; background-color: #302860; margin-left: " + std::to_string(size_dp(m, 3.0f)) + "dp\">&#9654;</div>";
        }
        rml += "<br/>" + (pieces ? en_rml : escape(vt.token->gloss));
        if (!vt.token->note.empty()) {
            rml += "<br/><span style=\"color: #b0b8d0\">" + escape(vt.token->note) + "</span>";
        }
        rml += std::string("<br/><span style=\"color: #8088a8\">") + (pinned ? "click again to close" : "click to keep open") + "</span>";
        tip->SetInnerRML(rml);
        overlay_doc->AppendChild(std::move(tip));
    }

    void hide_tooltip() {
        pair_count = 0;
        active_pair = -1;
        Rml::Element* old = overlay_doc->GetElementById("tooltip");
        if (old) {
            old->GetParentNode()->RemoveChild(old);
        }
    }

    bool same_layout(const goemon64::dialogue::View& a, const goemon64::dialogue::View& b) {
        if (a.page != b.page || a.tokens.size() != b.tokens.size() || a.rubies.size() != b.rubies.size()) {
            return false;
        }
        auto same_rects = [](const std::vector<goemon64::dialogue::Rect>& x, const std::vector<goemon64::dialogue::Rect>& y) {
            if (x.size() != y.size()) {
                return false;
            }
            for (size_t i = 0; i < x.size(); i++) {
                if (x[i].x != y[i].x || x[i].y != y[i].y || x[i].w != y[i].w) {
                    return false;
                }
            }
            return true;
        };
        for (size_t i = 0; i < a.rubies.size(); i++) {
            if (a.rubies[i].ruby != b.rubies[i].ruby || !same_rects(a.rubies[i].rects, b.rubies[i].rects)) {
                return false;
            }
        }
        for (size_t i = 0; i < a.tokens.size(); i++) {
            if (a.tokens[i].token != b.tokens[i].token || !same_rects(a.tokens[i].rects, b.tokens[i].rects)) {
                return false;
            }
        }
        return a.box.x == b.box.x && a.box.y == b.box.y && a.box.w == b.box.w && a.box.h == b.box.h;
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
    if (!view) {
        overlay_doc->Hide();
        shown_view_ref = nullptr;
        hovered_token = -1;
        pinned_token = nullptr;
        prev_left_down = false;
        goemon64::tts::stop();
        return;
    }

    Mapping m;
    m.scale = std::min(fb_width / canvas_width, fb_height / canvas_height);
    m.offset_x = (fb_width - canvas_width * m.scale) * 0.5f + canvas_pad_x * m.scale;
    m.offset_y = (fb_height - canvas_height * m.scale) * 0.5f + canvas_pad_y * m.scale;
    m.dp_ratio = context->GetDensityIndependentPixelRatio();

    const bool changed = !shown_view_ref || !same_layout(*shown_view_ref, *view);
    if (changed) {
        rebuild(*view, m);
        hovered_token = -1;
    }
    shown_view_ref = view;

    // Mouse position and clicks are polled so the overlay does not need to take over input from the game.
    int mx = 0, my = 0;
    SDL_Window* sdl_window = SDL_GetMouseFocus();
    int hit = -1;
    bool over_icon = false;
    bool left_down = false;
    float px = -1.0f, py = -1.0f;
    if (sdl_window) {
        left_down = (SDL_GetMouseState(&mx, &my) & SDL_BUTTON_LMASK) != 0;
        int win_w = 1, win_h = 1;
        SDL_GetWindowSize(sdl_window, &win_w, &win_h);
        px = mx * (float)fb_width / win_w;
        py = my * (float)fb_height / win_h;
        float gx = (px - m.offset_x) / m.scale;
        float gy = (py - m.offset_y) / m.scale;
        auto inside = [&](const goemon64::dialogue::Rect& r) { return gx >= r.x && gx < r.x + r.w && gy >= r.y && gy < r.y + r.h; };
        for (size_t i = 0; i < view->tokens.size() && hit < 0; i++) {
            for (const auto& r : view->tokens[i].rects) {
                if (inside(r)) {
                    hit = (int)i;
                    break;
                }
            }
        }
        if (view->page && !view->page->translation.empty()) {
            over_icon = inside(icon_rect(*view));
        }
    }

    if (left_down && !prev_left_down) {
        // The tooltip is drawn over the buttons under the dialogue box, so it gets the click first.
        const bool over_tooltip = over_element(overlay_doc->GetElementById("tooltip"), px, py);
        if (over_tooltip) {
            if (hovered_token >= 0 && hovered_token < (int)view->tokens.size() && over_element(overlay_doc->GetElementById("tip_play"), px, py)) {
                goemon64::tts::play(view->tokens[hovered_token].token->audio_key);
            }
        } else if (over_icon) {
            translation_open = !translation_open;
            apply_panel_visibility();
        } else if (view->page && over_element(overlay_doc->GetElementById("full_play"), px, py)) {
            goemon64::tts::play(view->page->audio_key);
        } else if (hit >= 0) {
            const auto* clicked = view->tokens[hit].token;
            pinned_token = (pinned_token == clicked) ? nullptr : clicked;
        }
    }
    prev_left_down = left_down;

    // A pinned phrase keeps its tooltip open (and its play button reachable) until it is clicked again.
    int pinned_idx = -1;
    if (pinned_token) {
        for (size_t i = 0; i < view->tokens.size(); i++) {
            if (view->tokens[i].token == pinned_token && !view->tokens[i].rects.empty()) {
                pinned_idx = (int)i;
                break;
            }
        }
        if (pinned_idx < 0) {
            pinned_token = nullptr;
        }
    }
    const int show = pinned_idx >= 0 ? pinned_idx : hit;
    const bool pinned = pinned_idx >= 0;

    if (show != hovered_token || pinned != shown_pinned || changed) {
        hovered_token = show;
        shown_pinned = pinned;
        if (show >= 0) {
            show_tooltip(view->tokens[show], m, pinned);
        } else {
            hide_tooltip();
        }
        if (view->page) {
            update_translation_text(view->page->translation, show >= 0 ? view->tokens[show].token->gloss : std::string());
        }
    }

    // Hovering a Japanese or English piece of the pinned tooltip highlights the matching piece in the other language.
    if (shown_pinned && pair_count > 0) {
        int pair = -1;
        for (size_t i = 0; i < pair_count && pair < 0; i++) {
            if (over_element(overlay_doc->GetElementById("jp_" + std::to_string(i)), px, py) ||
                over_element(overlay_doc->GetElementById("en_" + std::to_string(i)), px, py)) {
                pair = (int)i;
            }
        }
        if (pair != active_pair) {
            set_active_pair(pair);
        }
    }

    overlay_doc->Show();
}
