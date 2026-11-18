---
name: winui3
description: WinUI 3 / Windows App SDK pitfalls and patterns (C++/WinRT) - the CheckBox showing a stray "-", fixed-width windows sized to their content, keeping a busy UI cheap. Use when building or fixing a WinUI 3 desktop window.
---

# WinUI 3

## CheckBox shows "-" until hovered

WinUI's CheckBox draws its mark with an `AnimatedIcon`, which sometimes shows a leftover frame (a "-") until
the pointer goes over it. Fix: copy `DefaultCheckBoxStyle` from WinUI's `generic.xaml` (same Windows App SDK
version) into `App.xaml` as the implicit CheckBox style, and replace the `AnimatedIcon` with a `FontIcon`
named `CheckGlyph` (`Glyph="{StaticResource CheckBoxCheckedGlyph}"`, `Opacity="0"`), shown per visual state
with `CheckGlyph.Opacity` setters. Keep
the MIT notice of microsoft-ui-xaml in a comment above it, and list it with the third-party licenses.

## Fixed width, height from the content

- Width: `AppWindow().ResizeClient()` with the width in effective pixels times `GetDpiForWindow(hwnd) / 96.0`.
  Then on the `OverlappedPresenter`: `IsMaximizable(false)`, and `PreferredMinimumWidth` =
  `PreferredMaximumWidth` = client width + frame width.
- Height: after each layout pass (`LayoutUpdated` on the root panel), measure how tall the content wants to be.
  Its actual height is no use: a `ScrollViewer` stretches it to at least the window's height. Set the presenter's
  preferred maximum height to that, and if the window grew past the bottom of the work area, move it up.
- Give the root panel an explicit width (viewport width minus margins) from the ScrollViewer's `SizeChanged`.
  `HorizontalAlignment="Stretch"` inside a ScrollViewer centers it by its desired width, which pushes it
  sideways when nothing in it is that wide.

## A UI that updates 20 times a second

- Don't measure or update panels that aren't visible, or anything while the window is minimized or hidden;
  refresh a panel once when it's opened.
- Remember the last text or tooltip set on a control and only set it when it changes: each set costs a layout.
- Short button labels ("Switch") with the full meaning in `AutomationProperties.Name`.

## Notices

`InfoBar` (severity Informational) for update and status notices, with an action button. Closing it should hide
it for good where that makes sense (store what was closed).

The Windows App SDK runtime comes with Microsoft's license terms, which an installer has to pass on: see
`open-source-legal`.
