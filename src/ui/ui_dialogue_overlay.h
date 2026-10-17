#ifndef __UI_DIALOGUE_OVERLAY_H__
#define __UI_DIALOGUE_OVERLAY_H__

namespace Rml {
    class Context;
}

namespace goemon64 {
    // True while a dialogue box with database entries is on screen.
    bool dialogue_overlay_wanted();

    // Shows or hides the furigana and hover tooltips; call every frame before the RmlUi context is updated.
    void update_dialogue_overlay(Rml::Context* context, int fb_width, int fb_height, bool wanted);
}

#endif
