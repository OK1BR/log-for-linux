# log-for-linux

Native ham radio logbook for Linux (OK1BR). Third app of the family with
`sdr-for-linux` and `skimmer-for-linux` — same conventions apply:

- Plain **C11**, GTK4 + libadwaita, meson. No Rust/Python in the app.
- **Engine is GLib-only** (`src/engine/`, no GTK includes) — headless and
  testable; the GTK front-end lives in `src/app/`.
- App id: `cz.ok1br.log_for_linux`. License: GPL-3.0-or-later.
- Build: `meson setup builddir && meson compile -C builddir`.
- On Arch always build from source; install goes to `~/.local`, not `/usr`.

There is no design doc. The general ideas (why the app exists, the founding
design decisions) are in `README.md`; the plan is GitHub Issues; **the reason
for a decision is a comment next to the code it governs** — a comment never
points at a document. How things were built is the git history. `docs/SCOPE.md`
left the tree on 2026-09-18; its last version is at commit 2ad9864, the last
one with the full milestone walk-through at 372d390.

Work queue: **GitHub Issues** (`gh issue list`) since 2026-09-18 — bugs,
ideas, debt, and anything still waiting for a live check. Nothing of that
kind goes into `docs/`.
- Labels: type `bug` / `enhancement` / `debt`; `severity: high` = wrong data
  or something that leaves the machine wrong, `medium` = gets in the
  operator's way, `low` = cosmetic or log noise; `needs-live-check` = done
  in code, gate green, but the issue **stays open until the behaviour was
  seen live**; `at-the-radio` = the check needs the rig.
- A commit closes its issue with `Fixes #N` only when nothing is left to
  verify live — otherwise it says `Refs #N` and the issue is closed by hand
  once the check passed.
- Notes from live operation (contest days) are taken with no code touched,
  then triaged into issues. A feature too big for one issue body is split into
  several issues (a checklist in the parent), not written up as a doc.
- The pre-move backlog (LOG-1…LOG-5), the YO DX HF contest notes, the M3
  checklist and the RTTY zadání left the tree on 2026-09-18; their last
  versions are at commit 2a08503.
