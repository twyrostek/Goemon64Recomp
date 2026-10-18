#ifndef __GOEMON_TTS_H__
#define __GOEMON_TTS_H__

#include <string>

namespace goemon64::tts {
    // True when assets/tts/<key>.wav exists.
    bool available(const std::string& key);

    // Plays the recording on its own audio device, replacing anything that is still playing.
    void play(const std::string& key);

    void stop();

    // Gain for the game's own audio, lowered while a recording is playing. Call once per queued audio chunk.
    float duck_gain();
}

#endif
