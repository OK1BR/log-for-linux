# Log for Linux

**A native GTK4/libadwaita ham radio logbook for Linux.** The third app in the
family around [`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux) and
[`skimmer-for-linux`](https://github.com/OK1BR/skimmer-for-linux), sharing
their technology and architecture: a headless, GLib-only engine under a
GTK4/libadwaita front-end, plain C, meson. Replaces the earlier Rust
prototype (BRlog).

> **Status:** M0–M6 offline gates green (store, ADIF, UI, TCI, macros v2,
> **WSJT-X UDP**). QSO table: **click edits cell**, right-click
> delete (no row select). M3 manual UI gate:
> [`docs/M3-CHECKLIST.md`](docs/M3-CHECKLIST.md). TCI → `sdr-for-linux`
> `ws://127.0.0.1:40001` (VFO prefill + CW macros). WSJT-X/JTDX UDP on
> `127.0.0.1:2237`. Scope: [`docs/SCOPE.md`](docs/SCOPE.md).

## Build

```
meson setup builddir
meson compile -C builddir
meson test -C builddir
./builddir/log-for-linux
```

On Arch, install into the user prefix (not `/usr`):

```
meson setup builddir --prefix=$HOME/.local
meson compile -C builddir
meson install -C builddir
```

SQLite log: `~/.local/share/log-for-linux/log.db` (WAL). Preferences:
`~/.config/log-for-linux/settings.ini` (TCI, station callsign, macros/ESM,
WSJT-X enable/port) — menu → Preferences. ADIF is interchange only
(import/export from the window menu).

C11 · GTK4 · libadwaita · SQLite · meson · GPL-3.0-or-later
