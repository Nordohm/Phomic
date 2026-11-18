---
name: auto-gain
description: Automatic gain control for a voice stream, the way voice chat apps do it, with a limiter after it - measurements, time constants and the manual fallback. Use when a microphone stream is too quiet or uneven, or when building or tuning gain, AGC or a limiter.
---

# Automatic gain for voice

The goal: speech at a steady level, without the background swelling up in pauses and without clipping.

## Gain

Per block of samples (any size, 48 kHz mono float):

1. **Level:** mean square of the block, averaged with a time constant of 0.1 s, in dB.
2. **Noise floor:** from the block's own level. It follows down at once (time constant 0.05 s: the short gaps
   between words pull it down) and up slowly (15 s, so talking doesn't count as noise).
3. **Speech** is a level more than 10 dB above the noise floor. Only then does the gain move; in pauses it holds.
4. **Wanted gain** = target - level, with target -23 dBFS (RMS), clamped to 0…30 dB. Move toward it quickly when
   lowering (0.4 s) and slowly when raising (3 s).
5. **Apply it ramped** from the previous block's gain to the new one across the block, so a change doesn't click.
6. Skip digital silence (mean square below about 1e-11: muted, or nothing arriving). It says nothing about the
   room.

Time constants per block: `value += (target - value) * (1 - exp(-blockSeconds / timeConstant))`.

Start where it was last time (store it); the first time, at a sensible guess for the source. An iPhone's raw
microphone (measurement mode) is quiet: speech at arm's length peaks around -30 to -20 dBFS, so start at +18 dB.

## Limiter after it

The gain is too slow for laughs and shouts. A per-sample limiter catches them: ceiling 0.89 (-1 dBFS); when a
sample would go over, reduce at once to exactly the ceiling; recover with a time constant of about 100 ms. Then
the gain can be generous without the harsh distortion of clipping.

## Manual mode

A switch for automatic gain (on by default). When it's off, a gain slider appears: default 0 dB, with its range
(for example -12 … +30 dB) shown at the ends. While automatic, show only the current value ("Now +12 dB").

One gain and one limiter per stream (they keep state); not thread-safe.
