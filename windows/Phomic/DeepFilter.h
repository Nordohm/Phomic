#pragma once

// The C interface of windows/DeepFilter (Rust): DeepFilterNet3 with its model built in, for one mono 48 kHz
// stream. See DeepFilter/src/lib.rs.

#include <cstddef>

extern "C"
{
    struct PhomicDeepFilter;

    // Loads the model; takes a moment, so not on the audio thread. `attenLimitDb`: how far noise is turned
    // down at most (100: as far as it goes). Null if the model can't be loaded.
    PhomicDeepFilter* phomic_df_create(float attenLimitDb);

    // Samples per frame (480, 10 ms).
    size_t phomic_df_frame_length(PhomicDeepFilter const* state);

    // One frame. Frames under -70 dBFS come out silent, unheard. False if it couldn't be processed
    // (`output` then holds `input`).
    bool phomic_df_process(PhomicDeepFilter* state, float const* input, float* output);

    void phomic_df_free(PhomicDeepFilter* state);
}
