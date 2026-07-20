# Log for Linux

**A native GTK4/libadwaita ham radio logbook for Linux.** The third app in the
family around [`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux) and
[`skimmer-for-linux`](https://github.com/OK1BR/skimmer-for-linux), sharing
their technology and architecture: a headless, GLib-only engine under a
GTK4/libadwaita front-end, plain C, meson. Replaces the earlier Rust
prototype (BRlog).

> **Status: freshly started — project skeleton only.** Scope and design will
> land in `docs/SCOPE.md`.

## Build

```
meson setup builddir
meson compile -C builddir
./builddir/log-for-linux
```

C11 · GTK4 · libadwaita · meson · GPL-3.0-or-later
