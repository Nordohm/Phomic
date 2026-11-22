//! A C interface to DeepFilterNet for Phomic (see Phomic/DeepFilter.h): one mono 48 kHz stream, the
//! DeepFilterNet3 model built in. DeepFilterNet's own C interface (libDF's `capi`) loads the model from a
//! file instead, and stops the program when anything goes wrong; here a failure returns an error and
//! Phomic carries on without noise suppression.

use std::panic::{catch_unwind, AssertUnwindSafe};

use df::tract::{DfParams, DfTract, RuntimeParams};
use ndarray::{ArrayView2, ArrayViewMut2};

pub struct PhomicDeepFilter {
    model: DfTract,
}

/// Where the model's own estimate of the speech's SNR has it give up on a frame and send out silence
/// (libDF's default).
const NOISE_ONLY_DB: f32 = -10.;

/// Loads the model; takes a moment (some hundred milliseconds), so not on the audio thread.
/// `atten_lim_db`: how far noise is turned down at most, in dB (100: as far as it goes).
/// Returns null if the model can't be loaded.
#[no_mangle]
pub extern "C" fn phomic_df_create(atten_lim_db: f32) -> *mut PhomicDeepFilter {
    catch_unwind(|| {
        // Both stages on every frame. By default libDF skips its second stage (deep filtering, below
        // 5 kHz) when it thinks there's little noise (SNR over 20 dB), and both over 30 dB, to save time.
        // The first stage alone garbles the voice, and with a quiet room it switches back and forth.
        let params = RuntimeParams::default_with_ch(1)
            .with_atten_lim(atten_lim_db)
            .with_thresholds(NOISE_ONLY_DB, f32::INFINITY, f32::INFINITY);
        let model = DfTract::new(DfParams::default(), &params).ok()?;
        Some(Box::into_raw(Box::new(PhomicDeepFilter { model })))
    })
    .ok()
    .flatten()
    .unwrap_or(std::ptr::null_mut())
}

/// Samples per frame (480, 10 ms at 48 kHz).
#[no_mangle]
pub unsafe extern "C" fn phomic_df_frame_length(state: *const PhomicDeepFilter) -> usize {
    state.as_ref().map_or(0, |s| s.model.hop_size)
}

/// One frame: `input` and `output` hold `phomic_df_frame_length` samples. Frames quieter than
/// -70 dBFS come out silent and the model doesn't hear them, so quiet speech has to be turned up first
/// (Phomic/Denoiser.h). Returns false if the frame couldn't be processed (`output` then holds `input`).
#[no_mangle]
pub unsafe extern "C" fn phomic_df_process(
    state: *mut PhomicDeepFilter,
    input: *const f32,
    output: *mut f32,
) -> bool {
    let Some(state) = state.as_mut() else { return false };
    let n = state.model.hop_size;
    let ok = catch_unwind(AssertUnwindSafe(|| {
        let noisy = ArrayView2::from_shape_ptr((1, n), input);
        let enhanced = ArrayViewMut2::from_shape_ptr((1, n), output);
        state.model.process(noisy, enhanced).is_ok()
    }))
    .unwrap_or(false);
    if !ok {
        std::ptr::copy(input, output, n);
    }
    ok
}

#[no_mangle]
pub unsafe extern "C" fn phomic_df_free(state: *mut PhomicDeepFilter) {
    if !state.is_null() {
        drop(Box::from_raw(state));
    }
}
