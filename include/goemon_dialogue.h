#ifndef __GOEMON_DIALOGUE_H__
#define __GOEMON_DIALOGUE_H__

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace goemon64::dialogue {
    // Furigana for a kanji word.
    struct Ruby {
        uint32_t code_start; // index of the first glyph code in the segment
        uint32_t code_len;
        std::string reading; // hiragana
    };

    // A phrase with its meaning in context.
    struct Token {
        uint32_t code_start;
        uint32_t code_len;
        std::string surface;
        std::string reading; // hiragana, empty when the phrase has no kanji
        std::string gloss;
        std::string note;
    };

    // One line of text as the game prints it.
    struct Segment {
        uint32_t code_count;
        std::vector<Ruby> ruby;
        std::vector<Token> tokens;
    };

    // One dialogue box: its lines and the full translation.
    struct Page {
        std::string translation;
        std::vector<uint64_t> segment_hashes;
        std::vector<std::shared_ptr<const Segment>> segments;
    };

    struct Rect {
        float x, y, w, h; // game screen coordinates
    };

    struct ViewToken {
        const Token* token;
        std::vector<Rect> rects; // one per text line the phrase covers
    };

    struct ViewRuby {
        const Ruby* ruby;
        std::vector<Rect> rects;
    };

    struct View {
        std::shared_ptr<const Page> page;
        std::vector<std::shared_ptr<const Segment>> segments; // keeps the tokens alive
        std::vector<ViewToken> tokens;
        std::vector<ViewRuby> rubies;
        Rect box; // the dialogue window
    };

    // True while the window showing the current page is being updated by the game.
    bool is_active();

    // The page being shown with on-screen rectangles; null when it has no database entry.
    std::shared_ptr<const View> current_view();
}

#endif
