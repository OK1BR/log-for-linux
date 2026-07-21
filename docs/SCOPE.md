# Log for Linux — scope & plan

Goal: a native Linux **ham radio logbook** — the third app of the family around
**[`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux)** (SDR transceiver)
and **[`skimmer-for-linux`](https://github.com/OK1BR/skimmer-for-linux)**
(CW/RTTY/PSK skimmer), sharing their stack and architecture: a headless,
**GLib-only engine** (`src/engine/`, no GTK includes) under a GTK4/libadwaita
front-end, plain C11, meson. Successor of the deleted Rust prototype (BRlog).

Author: Richard Fakenberg, **OK1BR**. Licence: GPL-3.0-or-later.

## Why this exists

The Linux logbook landscape is either aging (CQRLOG — Lazarus/Pascal on a
MySQL/MariaDB daemon), non-native to this desktop (KLog — Qt), specialised
(TLF — ncurses contest logger) or web-based (Wavelog/Cloudlog). There is no
modern native GTK4/libadwaita logbook. And none of them can talk to the rest of
*this* family: `sdr-for-linux` exposes a proven TCI server (frequency/mode
state, spot rendering, click-to-tune), which makes radio-aware logging — the
entry row always pre-filled with the live VFO — nearly free.

## Decisions (2026-07-20, with Richard)

- **Canonical store: SQLite.** A logbook is a database workload — worked-B4
  lookups, dedup on import, per-band/mode statistics — and FT8 era logs run to
  tens of thousands of QSOs. A canonical ADIF flat file would mean parsing the
  whole log at startup, rewriting the file on every QSO and slow queries;
  SQLite is a single C library (no daemon), one file under
  `~/.local/share/log-for-linux/`, WAL mode survives crashes. **ADIF stays as
  the interchange format** (import/export), not the store.
- **v1 is the minimal usable logbook**: entry row + QSO table + search/filter +
  ADIF import/export + dup check. Everything else lands in later phases, each
  independently testable — the `skimmer-for-linux` milestone discipline.
- **Integrations in scope** (order refined below): **TCI client** to
  `sdr-for-linux`, **WSJT-X UDP** auto-logging, **callbook lookup**
  (QRZ.com/HamQTH) and **QSL sync** (LoTW via `tqsl`, eQSL, Club Log).
- **Not in scope** (decided 2026-07-20): a cluster/telnet spot client. The
  skimmer's M6 telnet feed (port 7300) was built to serve local loggers, so
  the hook exists on the other side if this is ever revisited — but this app
  does not grow a spot window now.
- **Clean start**: the old BRlog data is gone; no legacy-import pressure on the
  early phases. ADIF import is still an early milestone — it is the interchange
  path for everything (WSJT-X, QSL services, other loggers).

## Architecture

```
 src/engine/  (GLib-only, headless, testable)
    ├─ log store       sqlite3 · schema + CRUD + queries (worked-B4, dup check, stats)
    ├─ ADIF            parser + writer · lossless round-trip (unknown fields preserved)
    ├─ TCI client      libwebsockets · live VFO/mode from sdr-for-linux, QSY on demand
    ├─ WSJT-X UDP      GSocket server · QSO-logged ingest, worked-B4 replies
    ├─ callbook        libsoup · QRZ.com / HamQTH XML lookup
    └─ QSL sync        tqsl subprocess (LoTW) · eQSL · Club Log upload/download
 src/app/     (GTK4 + libadwaita)
    └─ main window: entry row (pre-filled from TCI) + QSO table + search/filter
```

### Key design decision: ADIF round-trips losslessly

The store keeps first-class columns for the fields the app queries (call,
timestamp, band, freq, mode/submode, RSTs, grid, name, QTH, power, comment,
QSL states) — ADIF-aligned names, timestamps additionally as an integer UTC
epoch for sorting and range queries. Every ADIF field the app does *not* model
is preserved per-QSO in an extras column and written back verbatim on export:
importing someone's log and exporting it again must never silently drop data.

### Reuse from the family (same in-tree policy)

- **TCI client** — `skimmer-for-linux` `src/engine/tci_client.c` (libwebsockets,
  own service thread, text split on `;`) is the direct template; the logbook
  needs only the text plane (vfo/mode/tx state), no IQ.
- **House test style** — every milestone ships an offline/headless gate binary
  (`log-*-test`), meson `test()`, live checks only where a counterpart app or
  service is genuinely required.

## Milestones

- **M0 — scaffold. IMPLEMENTED 2026-07-20.** Engine/app split, sqlite3
  dependency, test harness.
  Gate: `meson test` runs a trivial engine test; the empty window launches.
  Done as `src/engine/engine.c` (identity + sqlite end-to-end selfcheck),
  gate `log-engine-test`; the app links the engine and shows its versions.
- **M1 — log store. IMPLEMENTED 2026-07-20 (offline gate green).** Schema v1 +
  migrations, CRUD, worked-B4 query, dup check (same call+band+mode within a
  time window), stats counters.
  Gate: `log-store-test` — headless, covers schema migration from an empty and
  a v1 file, CRUD, dup/worked-B4 edge cases, and volume (50k synthetic QSOs).
  Done as `src/engine/log_store.c`: WAL, `PRAGMA user_version` migrations
  (refuses newer-than-app files), normalized call/band/mode (upper/lower/
  upper), NULL-for-unset columns, `extras` column reserved for the M2
  lossless round-trip, explicit tx API for bulk import, list with
  text/band/mode filters + paging. Measured (in-memory): 50k inserts in
  ~0.2 s inside one tx; worked-B4 + dup + list-20 in 0.1 ms. The app opens
  the real store (`~/.local/share/log-for-linux/log.db`) and shows the
  counters on the status page until M3 brings the UI.
- **M2 — ADIF import/export. IMPLEMENTED 2026-07-20 (offline gate green).**
  ADIF 3.1.x parser + writer over the store, unknown-field preservation,
  import dedup report.
  Gate: `log-adif-test` — round-trip byte-fidelity on modeled and unknown
  fields, plus a quirks corpus (real-world exports: case, line endings,
  missing `<EOH>`, odd whitespace).
  Done as `src/engine/adif.c`. Parser: byte-exact length-prefixed tag walk,
  tolerant of lowercase tags, CRLF, `:type` suffixes, inter-field garbage,
  a missing `<EOH>` (header iff the first non-ws char isn't `<`), 4-digit
  TIME_ON and a final record without `<EOR>`; a truncated declared length
  marks the record bad rather than eating the next one. Unmodeled fields →
  `extras` verbatim; BAND falls back to a freq→band table (2190m–23cm);
  bad records are counted, never abort; import is one tx with dup skipping
  (window 0 = exact-ts only). Writer: deterministic header (no timestamp —
  same log ⇒ byte-identical file), fixed field order, locale-safe trimmed
  numbers, oldest-first. Gate covers the quirks corpus, UTF-8 names,
  multiline comments, intra-file + re-import dedup accounting and the
  export(import(x)) byte-stability check.
- **M3 — UI v1: the usable logbook. CODE LANDED 2026-07-20; polish
  2026-07-21; the manual checklist gate awaits Richard
  (`docs/M3-CHECKLIST.md`).** Entry row (call/RST/band/mode/…,
  UTC clock), QSO table (GtkColumnView, newest first), search/filter,
  ADIF import/export in the UI, worked-B4 indication while typing.
  Gate: manual checklist — this milestone is deliberately UI; everything under
  it is already gated headless. **This is v1** once the checklist is signed off.
  Done as `src/app/win.c` (+ `qso_row.c` GObject row wrapper): entry row
  with UTC clock, live worked-B4 hint (green "New call" / yellow B4 counts
  per band/band+mode with last-worked date), RST defaults per mode
  (phone 59 / CW-like 599 / FT8·FT4 blank; never stomping a hand-edited
  report), freq → band dropdown auto-sync via the M2 band table (dropdown
  lists every ADIF band the engine knows), Enter anywhere logs; a 5-min
  same call+band+mode duplicate asks first (default Cancel). Table is a
  QSO table lives in a **separate hide-on-close window** (header icon
  `view-list-symbolic` before the hamburger; also menu “QSO log”) —
  virtualized GtkColumnView (newest first), search with 250 ms debounce,
  destructive-confirm delete; main window is the entry row + TCI/status.
  ADIF import/export via GtkFileDialog + GFile I/O with a result toast;
  import dedup is exact-timestamp only; QSO+calls counters in both window
  subtitles. Store open failure surfaces a dialog instead of aborting.
- **M4 — TCI integration. CODE LANDED 2026-07-21 (offline gate green).**
  Connect to `sdr-for-linux` (`ws://127.0.0.1:40001`), entry row pre-fills
  freq/mode from the live VFO, double-click on a logged QSO → QSY.
  Gate: `log-tci-test` — mock TCI server (skimmer house pattern); live check
  against the real radio when available.
  Done as `src/engine/tci_client.c` (libwebsockets, text plane only — no IQ):
  handshake to `ready;`, track `vfo`/`modulation`/`device`/`protocol`, explicit
  `tune()` for QSY, mode map into the logbook dropdown (cw→CW, usb/lsb→SSB,
  digu/digl→FT8, …). UI (`win.c`): background connect + 5 s reconnect, status
  line next to the UTC clock, auto-prefill of MHz/band/mode from the radio,
  double-click / activate on a table row QSYs when frequency is set. TCI
  host/port (and station callsign) live in Preferences → GKeyFile
  `~/.config/log-for-linux/settings.ini` (`src/app/settings.c`, skimmer/sdr
  house style: `AdwPreferencesDialog`, save on dialog close; host/port change
  reconnects immediately). Entry window has an N1MM-inspired **macro bar v1**
  (F1–F8 + Esc stop): fixed CQ/EXCH/TU/MY/HIS/AGN/QRZ/STOP with `{MYCALL}`
  `{CALL}` `{RST}` expansion; CW text via TCI `cw_macros` to sdr-for-linux
  when connected. Full editable macros / Run·S&P / ESM → **M5**. The logbook
  never changes radio state except explicit user QSY and those
  operator-triggered CW macros.
- **M5 — macros v2 (contest-style messaging). IMPLEMENTED 2026-07-21
  (offline gate green).** Grew the F-key strip into a messaging layer
  (inspired by N1MM+, not a clone). Done:
  1. **Editable macros** — right-click F-key → caption + CW template dialog;
     persist in `settings.ini` groups `macros_run` / `macros_snp`; tokens
     `{MYCALL}` `{CALL}` `{RST}` and `!` via engine `logfl_macro_expand`.
  2. **Run vs S&P** — two 8-key banks with distinct defaults; Run/S&P
     toggles on the entry window; active bank persisted as `contest.bank`.
  3. **ESM — Enter sends message** — optional (`contest.esm` / Preferences);
     Enter advances READY→EXCH→LOG→TU (empty call → CQ; after log with
     cleared call still TU). **Log QSO** button always logs. Off by default
     so daily logging keeps Enter = log.
  4. **SSB “wav” / DVK — OUT OF SCOPE** (unchanged): CW text via TCI only.
  Gate: `log-macro-test` — expand, bank defaults/edit, ESM transitions.
  Live CW smoke against sdr-for-linux when available (not formal signed gate).
- **M6 — WSJT-X UDP.** UDP server (default port 2237): decode `QSO Logged`
  into the store, answer status/worked-B4 (callsign highlight).
  Gate: `log-udp-test` — replay captured WSJT-X datagrams headless; live check
  logging one real FT8 QSO end to end.
- **M7 — callbook lookup.** QRZ.com XML (subscriber) / HamQTH (free) —
  name/QTH/grid auto-fill on callsign entry, on-disk cache, credentials in the
  keyring, never in config files.
  Gate: `log-callbook-test` against canned XML; live smoke against both APIs.
- **M8 — QSL sync.** LoTW: sign+upload via `tqsl` CLI, pull confirmations
  (`lotwreport.adi`) and mark QSLs; eQSL upload + inbox; Club Log upload.
  Per-QSO sent/confirmed state per service, retry-safe (idempotent re-upload).
  Gate: `log-qsl-test` over mocked endpoints; live check with a small batch.
- **Edit saved QSO. IMPLEMENTED 2026-07-21.** Decided 2026-07-21 (Richard):
  v1 originally only add/delete; correcting a logged QSO is first-class.
  UI: QSO log window pencil loads the row into the main entry row (`Save QSO`
  / `Cancel`); double-click still QSYs. Engine path is `logfl_store_get` +
  `logfl_store_update` so extras, QSL flags, original `ts`, grid/QTH/power
  and station fields stay intact while call/RST/band/mode/freq/name/comment
  can be fixed. TCI prefill is frozen while editing. Needed to correct early
  QSOs that were saved with `freq` NULL (band only) before the MHz
  prefill/TCI fallback landed.
- **Freq must be exact in SQL (caught 2026-07-21).** Schema always had
  `freq REAL`; the UI used to leave the MHz entry empty (placeholder only),
  so `bind_qso` stored NULL and only `band` survived. Fix direction: typed
  MHz → live TCI VFO at log time → band mid-point fallback; band change
  seeds MHz when empty; TCI still overwrites with the real VFO. Existing
  NULL rows need edit-QSO or a one-shot repair later.
- **Later** — DXCC/awards tracking (cty.dat entity resolver, worked/confirmed
  matrices per band/mode), contest mode (serials, Cabrillo export), and — only
  if ever revisited — the skimmer cluster client.

## Safety / etiquette

The logbook never transmits and never changes radio state except an explicit
user QSY (M4 double-click). Nothing leaves the machine without an explicit
action or an explicitly enabled sync service; callbook/QSL credentials live in
the secret service, not in config files. Richard's global rule applies: consent
before any major or irreversible step — a destructive log operation (mass
delete, re-import over existing data) always confirms first, and every upload
to an external QSL service is user-triggered in v1 (no silent background sync).
