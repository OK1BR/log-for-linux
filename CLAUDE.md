# log-for-linux

Native ham radio logbook for Linux (OK1BR). Third app of the family with
`sdr-for-linux` and `skimmer-for-linux` — same conventions apply:

- Plain **C11**, GTK4 + libadwaita, meson. No Rust/Python in the app.
- **Engine is GLib-only** (`src/engine/`, no GTK includes) — headless and
  testable; the GTK front-end lives in `src/app/`.
- App id: `cz.ok1br.log_for_linux`. License: GPL-3.0-or-later.
- Build: `meson setup builddir && meson compile -C builddir`.
- On Arch always build from source; install goes to `~/.local`, not `/usr`.

Scope/design: `docs/SCOPE.md` (written 2026-07-20 — read it first; it holds
the decided store/integrations/milestones).

RTTY keying: `docs/RTTY-SCOPE.md` (2026-08-15) — implemented same day
(§6 confirmed by Richard, steps 1–2 done, gate green); the live pass
against sdr-for-linux ≥ f38aafd (§7.3) is pending at the radio.
