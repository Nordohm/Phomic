---
name: github-releases
description: Shipping a desktop app through GitHub Releases - release branch, merge, tag, draft release with installer, publishing, and the Git Credential Manager account picker. Use when cutting, publishing or checking a release on GitHub, or when git push or the GitHub API hangs on Windows.
---

# Releasing on GitHub

## Flow

1. Work on a `release-X.Y` branch; bump the version in one place (a `Version.h` or similar) that the app, the
   installer and the file names all read.
2. Build the release files: an installer (`App-Setup-X.Y.Z.exe`) and a portable zip of the same files.
3. Create the release as a **draft** on tag `vX.Y.Z` with both files attached. Compare each asset's `digest`
   (`sha256:…` in the API) with the local files.
4. After testing: `git switch main && git merge --ff-only release-X.Y`, push.
5. Publish: `PATCH /repos/<owner>/<repo>/releases/<id>` with `{"draft": false, "target_commitish": "main",
   "make_latest": "true"}`.
6. Check that `https://github.com/<owner>/<repo>/releases/latest` redirects to the new tag. A download page should
   link there, not to a fixed version.
7. Delete the release branch locally and on GitHub once it's in main (`git merge-base --is-ancestor`). Releases
   hang on tags, not branches.

## Installer

- Show a license page (Inno Setup: `LicenseFile=`) with your license and a short summary of third-party terms the
  user agrees to.
- Ship the full license texts in a `licenses` folder next to the program.
- Don't bundle software you may not redistribute (drivers and the like): link to its vendor.

## Git Credential Manager with several GitHub accounts

Without a username, Git Credential Manager pops an account picker, which hangs non-interactive commands.

- Push: `GIT_TERMINAL_PROMPT=0 git -c credential.username=<account> push origin <branch>`.
- API token: `git credential fill` with `protocol=https`, `host=github.com`, `username=<account>`; never print it.
- Keep a brand or organization account apart from a personal one: commit as the brand (with its noreply
  email).

## CI

- Pushes to `main` can run a check build; an upload or release build runs on `workflow_dispatch` with an input.
- Limit `on.push.paths` to the part of the repo a workflow builds, so doc commits don't trigger it.
