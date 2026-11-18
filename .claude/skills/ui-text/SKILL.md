---
name: ui-text
description: Rules for writing UI text, READMEs and other user-facing prose. Use before adding or changing any label, caption, hint, banner, notice, slider, README section or support/donation line.
---

# UI text

## Don't write what the screen already says

Before adding a caption, subtitle, hint or banner, ask whether the controls already say it. If they do, leave it out.
Rejected examples:

- "Off: set the gain below" under a switch, with the slider right below it.
- The app's own name as a screen title.
- A transport or category name next to an item when the tab it sits in already says it.
- "One at a time. Pick X, Y or Z in …" footers that restate the picker above them.

Hidden accessibility text (names, hints) is fine: it's where the longer meaning goes.

## Short and exact

- A button says the action: "Switch", not "Switch here". Put the full meaning in the accessibility name
  ("Switch to Wi-Fi").
- Option names are one word or a number: "Automatic", not "Auto - adapts to the connection".
- Numbers instead of vague words: "Stable (120 ms)", not "a little more delay".
- A notice's title alone, when it says everything ("Version 1.2.0 is available"); the button does the rest.
- A live value shows only the value ("Now +12 dB"), not a sentence about it.

## Sliders

- Default at the neutral point (0 dB, 0 %, off), never offset from it.
- Show the minimum and maximum at the ends.
- With an automatic mode: a switch for automatic, and the manual slider only when it's off.

## READMEs

- What it is, how to install, what to do when it doesn't work, support, license. Move how it's built to `docs/`.
- One line per troubleshooting case: **Symptom:** what to do.
- A support or donation line invites, it doesn't order: "If you'd like to support X, you can buy me a coffee ☕",
  not "If X is useful to you, buy me a coffee". No full stop straight after an emoji.
- Credit or thank third parties where it helps the reader (install steps, license), not in the support section.

For store texts see the `app-store-release` skill; for how Markdown renders on GitHub, `github-markdown`.
