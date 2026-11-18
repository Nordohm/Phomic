---
name: github-markdown
description: How Markdown renders on github.com and how to check it before pushing. Use when writing or editing README.md or docs with tables, code blocks, badges or images.
---

# Markdown on GitHub

## Tables

A cell with a long token that can't wrap (JSON, a URL, a UUID with an example) makes its column as wide as the
token and squeezes the others into a few letters per line. Keep table cells to words; put such tokens in a code
block below, with notes as a bullet list. For a protocol, an example exchange works well:

```
→ {"type":"hello","version":1}
← {"type":"welcome","version":1}
```

## Code blocks

GitHub's file view has about 980 px for a code block: about 130 characters in Consolas (Windows), about
119 in SF Mono (Mac). Longer lines scroll sideways. Keep lines under about 115 characters.

## Badges and images

- An image alone doesn't link anywhere; clicking it opens the image file. Wrap it:
  `[![Download on the App Store](badge.svg)](https://apps.apple.com/app/id<app id>)`.
- Size images with HTML: `<img src="qr.png" width="200" alt="…">`. Link it too if it stands for a URL.

## Check before pushing

Render with GitHub's own renderer, then look at it:

1. `POST https://api.github.com/markdown` with `{"text": <file>, "mode": "gfm", "context": "<owner>/<repo>"}`
   (authenticated). It returns the HTML fragment.
2. Wrap it in `<article class="markdown-body">` with
   `https://cdn.jsdelivr.net/npm/github-markdown-css@5.5.1/github-markdown-light.css`, width 1012 px.
3. Serve it from localhost (a browser pane can't script `file://`) and compare each `pre` and `table` element's
   `scrollWidth` with its `clientWidth`. Equal means nothing overflows.
