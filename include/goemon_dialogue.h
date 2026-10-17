#ifndef __GOEMON_DIALOGUE_H__
#define __GOEMON_DIALOGUE_H__

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace goemon64::dialogue {
    struct Token {
        uint32_t code_start; // index of the first glyph code of the token in the segment
        uint32_t code_len;
        std::string surface;
        std::string reading; // hiragana, empty when the surface has no kanji
        std::string gloss;
        std::string note;
    };

    struct Segment {
        uint32_t window; // game address of the text window that is printing it
        uint32_t code_count;
        std::vector<Token> tokens;
    };

    struct Rect {
        float x, y, w, h; // game screen coordinates (320x240)
    };

    struct ViewToken {
        const Token* token;
        std::vector<Rect> rects; // one per text line the token covers
    };

    struct View {
        std::vector<std::shared_ptr<const Segment>> segments; // keeps the tokens alive
        std::vector<ViewToken> tokens;
    };

    // Words of the segment the game printed last, with their on-screen rectangles; null when it has no database entry.
    std::shared_ptr<const View> current_view();

    // True while the window that is showing the current segment is being updated by the game.
    bool is_active();
}

#endif
