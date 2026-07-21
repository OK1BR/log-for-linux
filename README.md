# Log for Linux

**A native GTK4/libadwaita ham radio logbook for Linux.** The third app in the
family around [`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux) and
[`skimmer-for-linux`](https://github.com/OK1BR/skimmer-for-linux), sharing
their technology and architecture: a headless, GLib-only engine under a
GTK4/libadwaita front-end, plain C, meson. Replaces the earlier Rust
prototype (BRlog).

> **Status:** M0–M4 offline gates green (store, ADIF, UI, **TCI**). M3 manual
> UI gate: [`docs/M3-CHECKLIST.md`](docs/M3-CHECKLIST.md). TCI talks to
> `sdr-for-linux` on `ws://127.0.0.1:40001` (prefill VFO; double-click QSY).
> Scope: [`docs/SCOPE.md`](docs/SCOPE.md).

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

SQLite log: `~/.local/share/log-for-linux/log.db` (WAL). ADIF is interchange
only (import/export from the window menu).

C11 · GTK4 · libadwaita · SQLite · meson · GPL-3.0-or-later
