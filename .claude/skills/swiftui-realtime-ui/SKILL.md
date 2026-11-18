---
name: swiftui-realtime-ui
description: Keeping SwiftUI smooth with live values (audio levels, meters, glows) and slow system calls (audio session, audio units), plus color transitions that don't go muddy. Use when a SwiftUI animation stutters, a view redraws too often, or when blending colors by a level.
---

# Smooth SwiftUI with live values

## Fast-changing values

A value that changes 20 times a second (a level meter) re-renders every view that reads it. With `@Observable`,
read it only in a small leaf view that reads the model itself (`@Environment(Model.self)`), not in the screen
that contains it, and never pass it down as a parameter: then only the leaf redraws.

## Slow system calls off the main thread

Activating an `AVAudioSession` and starting or stopping an audio unit can take a few hundred milliseconds. On the
main thread that stalls the UI, and animations (a mute button) visibly stutter.

- Run them on a serial queue of their own. Number each request; when one starts, skip it if a newer one has
  arrived, so only the latest counts.
- Keep two states: what's wanted (set at once on the main actor, drawn by the UI) and what's actually running
  (reported back when done).
- Let callers that must wait (an App Intent that mutes) await an "idle" point.

## Smoothing

Ease values frame by frame (`TimelineView(.animation(minimumInterval: 1.0 / 30, paused: …))`) instead of
animating each change, with different speeds up and down: rise in about 0.1 s, fall in about 0.7 s, like a level
meter. Keep the smoother's state in a class, so it can change while the view is drawn. Pause the timeline when
nothing moves (and for Reduce Motion).

## Color by level

- Green to red through yellow and orange: interpolate **hue, saturation and brightness**, not RGB (RGB passes a
  muddy olive).
- Hue wraps around at 1. iOS 26's `systemRed` has a hue just under 1 (about 0.997), not just over 0, so going up to
  it from green passes through cyan and blue. If red's hue is above green's, subtract 1, interpolate, and add 1
  to a negative result.
- Start warming only near the top (for example the last 9 dB below full scale, eased with a smoothstep) and let
  it warm quickly and cool slowly.
- Resolve system colors for the current color scheme (`resolvedColor(with:)`) before reading their components.
