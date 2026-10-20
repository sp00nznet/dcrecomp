# Contributing

Issues and PRs are welcome. Two rules first:

- **Never attach or commit game data**: disc images, ROMs, BIOS, extracted
  files, or recompiler output (`src/game/`, `*_functions.json`). It is all
  derived from someone else's binary.
- **Keep title-specific values in the game repo.** The toolkit gets the general
  mechanism (a flag, a hook, an `--entries` file); the address stays with the
  title.

## Sending a change

1. Branch from `main`, named for the change (`feat/maple-vmu`, not `crazy-taxi-fixes`).
2. Build (`cmake -B build && cmake --build build`) and run what tests exist.
3. If you touched the recompiler, recompile a title before and after and say
   what changed (function counts, or a diff of the output).
4. In the PR: what was wrong, which title showed it, and whether the change
   alters behaviour for every title or sits behind a flag.

PRs merge with a merge commit, so your commits keep your name.

## Where your code comes from

dcrecomp is **GPL-2.0**. Parts of it are derived from Flycast (see `NOTICE`).

- Code you wrote yourself, or code under a GPL-2.0-compatible licence (MIT, BSD,
  ISC, zlib, GPL-2.0-or-later, LGPL-2.1) is fine. Say in the PR where any
  non-original code came from, and keep its licence header.
- **Not accepted:** GPL-3.0-only, AGPL, non-commercial licences (Snes9x-style,
  CC-BY-NC), and code with no licence at all - including most matching decomps.
  These can't be combined with GPL-2.0.
- **No leaked SDK material.** Nothing from the Katana or Naomi SDKs, their
  headers or their docs.
- If you took code or structure from Flycast, say so in the PR so it goes in
  `NOTICE`.

## AI-assisted contributions

Welcome, provided a person understood and verified the change before sending it.
