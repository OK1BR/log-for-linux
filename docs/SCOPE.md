# Log for Linux — scope & design

Goal: a native Linux **ham radio logbook** — the third app of the family around
**[`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux)** (SDR transceiver)
and **[`skimmer-for-linux`](https://github.com/OK1BR/skimmer-for-linux)**
(CW/RTTY/PSK skimmer), sharing their stack and architecture: a headless,
**GLib-only engine** (`src/engine/`, no GTK includes) under a GTK4/libadwaita
front-end, plain C11, meson. Successor of the deleted Rust prototype (BRlog).

Author: Richard Fakenberg, **OK1BR**. Licence: GPL-3.0-or-later.

This file holds the **decisions and their reasons** — what the app is, why it
is built that way, and what it deliberately does not do. What the app does is
the [README](../README.md); what is open or planned — bugs, ideas, pending live
checks, future milestones — lives in
[GitHub Issues](https://github.com/OK1BR/log-for-linux/issues); how each piece
was built is the git history. The last version that still walked through every
finished milestone in detail is
[commit 372d390](https://github.com/OK1BR/log-for-linux/blob/372d390239a45c1851c4b8bcd35a7983e5c16a17/docs/SCOPE.md).

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
- **Integrations in scope**: **TCI client** to `sdr-for-linux`, **WSJT-X UDP**
  auto-logging, **callbook lookup** (QRZ.com/HamQTH, #8) and **QSL sync** (LoTW
  via `tqsl`, eQSL, Club Log, #9).
- **Not in scope** (decided 2026-07-20): a cluster/telnet spot client. The
  skimmer's M6 telnet feed (port 7300) was built to serve local loggers, so
  the hook exists on the other side if this is ever revisited — but this app
  does not grow a spot window now.
- **Clean start**: the old BRlog data is gone; no legacy-import pressure.
  ADIF import is the interchange path for everything (WSJT-X, QSL services,
  other loggers).

## Architecture

```
 src/engine/  (GLib-only, headless, testable)
    ├─ log store       sqlite3 · schema + CRUD + queries (worked-B4, dup check, stats)
    ├─ ADIF            parser + writer · lossless round-trip (unknown fields preserved)
    ├─ TCI client      libwebsockets · live VFO/mode from sdr-for-linux, CW/RTTY keying
    ├─ WSJT-X UDP      GSocket server · QSO-logged ingest, worked-B4 replies
    ├─ contests        exchange templates · validity · score · Cabrillo · cty resolver
    ├─ dup service     UDP line protocol for skimmer-for-linux
    ├─ callbook        (M7, planned) libsoup · QRZ.com / HamQTH XML lookup
    └─ QSL sync        (M8, planned) tqsl subprocess (LoTW) · eQSL · Club Log
 src/app/     (GTK4 + libadwaita)
    └─ main window: entry row (pre-filled from TCI) + macro strip + QSO table
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

One entry per milestone: status, module, gate — and the decisions made in it
that the code alone does not explain. The M-labels are cited from the code.

- **M0 — scaffold. DONE.** Engine/app split, sqlite3 dependency, test
  harness (`log-engine-test`).
- **M1 — log store. DONE.** `src/engine/log_store.c`; gate `log-store-test`.
  Migrations by `PRAGMA user_version` — a newer-than-app file is refused.
- **M2 — ADIF import/export. DONE.** `src/engine/adif.c`, ADIF 3.1.x; gate
  `log-adif-test`. The parser tolerates real-world quirks; bad records are
  counted, never abort; import is one tx with dup skipping. The writer is
  deterministic (no timestamp in the header — same log ⇒ byte-identical
  file).
- **M3 — UI v1: the usable logbook. DONE — manual checklist signed off by
  Richard 2026-07-21.** `src/app/win.c` (+ `qso_row.c`).
  Enter anywhere logs; a 5-min same call+band+mode duplicate asks first
  (default Cancel); RST defaults per mode never stomp a hand-edited report;
  ADIF import dedup is exact-timestamp only; a store open failure surfaces a
  dialog instead of aborting.
- **M4 — TCI integration. DONE, in live contest use since EUHFC
  2026-08-01.** `src/engine/tci_client.c` (libwebsockets, text plane only —
  no IQ) to `sdr-for-linux` (`ws://127.0.0.1:40001`); gate `log-tci-test`
  (mock TCI server, skimmer house pattern).
  - The logbook never changes radio state except operator-triggered CW/RTTY
    macros and keyer speed.
  - Table-driven QSY was tried and dropped (2026-07-21): not useful enough
    for a toolbar control, and a click on a cell is reserved for inline
    edit; `tune()` stays in the engine API.
  - **Keyer speed:** Page Up/Down nudge WPM by 1 from anywhere in the entry
    window (contest-logger style, focus stays in Call), clamped 5–60 to
    match sdr-for-linux; the radio's echo (handshake, own controls, our
    set) is the authority.
  - **Spot-click prefill:** clicking a skimmer spot on the sdr-for-linux
    panadapter (`rx_clicked_on_spot` / `clicked_on_spot`, receiver 0 only)
    puts the callsign into Call, selected, and the worked-B4 / dup check
    runs on it as if typed. Spots themselves stay one-way client→server in
    TCI, so the logbook never sees the spot list — this is the tiny prefill
    the no-cluster-window rule leaves room for, not a spot window. The
    prefill stays a prefill until the operator types anywhere into the QSO
    row — Call, RST, exchange, Sent, Name or Comment. Until then clicking
    another spot replaces it, and tuning more than 200 Hz off the spot
    (that call is no longer on frequency) resets the WHOLE entry row, so
    leftovers of a QSO that never happened cannot leak into the next one.
    Once typed into, the row is the operator's: QSY never deletes it, a
    spot click never overwrites it, and an open cell editor is never
    interrupted.
  - The New-call / worked-B4 / DUP verdict line uses family colors: New =
    #30C060 (the skimmer's own spot green on the panadapter), DUP =
    saturated #ED333B.
- **M5 — macros v2 (contest-style messaging). DONE; in live contest use
  since EUHFC 2026-08-01.** The F-key strip (F1–F8 + Esc stop) is a
  messaging layer inspired by N1MM+, not a clone; expansion lives in the
  engine (`logfl_macro_expand`). Gate: `log-macro-test`.
  - Empty tokens collapse, so nothing changes outside contests.
  - **Run vs S&P** — two 8-key banks with distinct defaults.
  - **ESM** is optional and off by default so daily logging keeps Enter =
    log; the **Log QSO** button always logs.
  - **Cut numbers** are opt-in, applied at send time to RST/serial/all-digit
    exchange only, never callsigns, and only in CW; the log keeps real
    digits.
  - **Queued-message word gap:** text goes out with a leading space,
    SDC-style — sdr-for-linux's generator inserts the inter-message gap
    only when the following text asks for it (a trailing space does
    nothing; skipped on an idle keyer, so overs never start with dead
    air).
  - **SSB "wav" / DVK — OUT OF SCOPE**: text keying via TCI only.
- **RTTY keying. DONE 2026-08-15; in live contest use since SARTG WW RTTY
  2026-08-15/16.** The same F-key macros key a complete RTTY exchange
  through sdr-for-linux's TCI family extension `rtty_macros:0,<text>;` /
  `rtty_macros_stop;` (needs sdr-for-linux ≥ f38aafd; an older radio side
  ignores the unknown command and keys nothing — the family upgrades
  together; accepted, not handled). Macro bar, banks, ESM, placeholder
  expansion and the QSO entry flow are mode-agnostic and unchanged.
  Decisions (Richard, 2026-08-15):
  - Dispatch authority is the **log mode dropdown** (`CW` → `cw_macros`,
    `RTTY` → `rtty_macros`, any other mode refuses with a toast), not the
    radio's reported mode. The dropdown is prefilled from the radio —
    `modulation:0,rtty;` arrives on every change and in the handshake, so
    when the SDR lights up RTTY the dropdown must never sit on CW — and
    stays overridable. If it says RTTY while the radio sits in another
    mode, the radio side refuses and nothing transmits.
  - Cut numbers are suppressed in RTTY — digits go verbatim, no new
    setting.
  - Stop/Esc always fires BOTH `cw_macros_stop;` and `rtty_macros_stop;`
    (panic semantics; stopping an idle generator is a no-op on the radio).
  - The Ctrl+K text window is mode-aware; no separate RTTY window.
  - No RTTY speed UI (45.45 Bd is fixed radio-side); PgUp/PgDn stay CW WPM.
  - Same text treatment as CW: reserved `:`/`,`/`;` scrubbed to spaces, the
    deliberate leading space kept.
- **M6 — WSJT-X UDP. CODE DONE (offline gate green); the live check with a
  real FT8 QSO is still ahead.** `src/engine/wsjtx_udp.c`: GSocket server on the
  GLib main loop (default `127.0.0.1:2237`); `QSO Logged` is decoded into
  the store (exact-ts dup skip), `Status` is answered with worked-B4 via
  `Highlight Callsign`. Auto-logged QSOs always land in the main log — FT8
  contest support is #13, deliberately later. Gate: `log-udp-test`.
- **Dup lookup service for skimmer-for-linux. DONE, live-verified in EUHFC
  2026-08-01.** Read-only UDP line protocol on `127.0.0.1:2238`, always on
  with the app: `DUP? <call> <freq_hz> <mode>` → `NEW|B4|DUP|INV <call>`
  back to the sender. DUP = call+band+mode already in the ACTIVE contest
  (band derived from the frequency), B4 = worked any time, NEW = not in
  the log, INV = not a valid contest QSO (strongest skip; the skimmer
  paints it grey); malformed requests get silence, so the skimmer treats a
  timeout as unknown and spotting survives the logbook being closed.
  **Push:** the service remembers peers with a valid `DUP?` in the last
  10 min (8 slots) and any verdict-changing mutation sends them the same
  answer datagram unsolicited, so the skimmer regrays the live panadapter
  label at once instead of on its ≤180 s re-announce. The logbook
  deliberately never writes TCI `spot:` — the label color is the skimmer's
  to own (two writers would race). Gate: `log-dupq-test`. The skimmer side
  (querying, caching, coloring) lives in skimmer-for-linux (its SCOPE.md).
- **Hand-typed text is upper case as it is typed (Richard 2026-08-14;
  exchange added after YO DX HF 2026-08-22).** Call, the exchange fields
  and Sent — in the entry row and in the inline cell editor of a saved
  QSO — show capitals while being typed, whatever Caps Lock happens to be
  doing: the operator never looks at the keyboard mid-QSO and must see what
  will really go out.
- **Edit saved QSO. DONE (decided with Richard 2026-07-21).** Correcting a
  logged QSO is first-class and inline-only; the entry strip stays for
  **new** QSOs (no pencil / load-into-entry). No row selection: the theme's
  row hover marks the edit target and a single click on a cell opens the
  editor — single-click beats double-click, the hover then reads as "click
  to edit", not as a pointless flash. Enter commits — everything else
  discards: Esc, a click anywhere outside the cell, focus-out, scroll-away.
  Delete is right-click on the row → confirm dialog (no intermediate
  context menu). A single-cell edit keeps extras, QSL flags, grid/QTH/power
  and station fields intact.
- **Freq is always stored exactly.** `freq REAL` is filled from: typed MHz
  → live TCI VFO at log time → band mid-point fallback; a band change seeds
  MHz when empty; TCI overwrites with the real VFO.
- **M7 — callbook lookup. PLANNED — #8.**
- **M8 — QSL sync. PLANNED — #9.**
- **M9 — contest management. DONE (decided 2026-07-27 with Richard, pulled
  ahead of M7/M8).** Contests are first-class log sections: create/delete/
  switch, each contest with its own exchange template. Gate:
  `log-contest-test`.
  1. **One DB, `qso.contest_ref`** (NULL = main log) — contest QSOs stay in
     the canonical store (worked-B4/DXCC/LoTW see them); switching is a
     filter. Rejected: per-contest DB files (N1MM style) — would fragment
     worked-B4 and the canonical store.
  2. **Deleting a contest asks what to do with its QSOs**: delete them too,
     or unlink them into the main log (dialog with both options + counts).
  3. **Templates are generic, presets are a starting point**: a template
     lists received-exchange fields (serial / number / text / auto), each
     mapped onto ADIF (SRX, SRX_STRING, CQZ, ITUZ, …; unmodeled targets ride
     in extras as real ADIF tags). A contest copies its definition at
     creation — editing presets never rewrites history. Presets are added
     over time as needed, each verified against the sponsor's official
     rules first.
  4. **Main log view = non-contest QSOs only** (`contest_ref` NULL); a
     contest's QSOs are visible when switched into it. Worked-B4 and stats
     stay global. ADIF export always exports everything by default.

  ADIF: CONTEST_ID is written from the linked contest's `adif_id`; an
  imported CONTEST_ID stays verbatim in extras (no auto-created contests —
  deliberate; linking such QSOs afterwards is #11).
- **Cabrillo export. DONE.** `src/engine/cabrillo.c`: WWROF v3 shape
  verified against wwrof.org and the official EUHFC sample (log.s5cc.eu);
  gate `log-cabrillo-test`. Exports the ACTIVE contest. **CATEGORY-MODE and
  CATEGORY-BAND are derived from the contest's own QSOs** (a remembered
  RTTY from SARTG once mislabelled the all-CW YO DX entry): mode families
  collapse to MIXED, bands to ALL, the remembered value is only a fallback
  for a contest with no QSOs yet, and both rows stay editable because the
  log proves what was worked, not which category was entered.
  CLAIMED-SCORE is prefilled from the score estimate below (editable, never
  persisted).
- **cty resolver + contest validity rules. DONE (triggered mid-WAE
  2026-08-08: an EU station gave Richard no serial — WAE scores EU↔non-EU
  only, and the log had no idea).** `src/engine/cty.c`: parser for the AD1C
  cty.dat (MIT; snapshot + license note in `data/`, installed to the app
  data dir, user-replaceable); gate `log-cty-test`. Presets carry a
  validity rule in exch_def (`counts=all|eu-dx|eu-only`,
  `zero_own_country`), each verified against the sponsor's official rules
  (house rule: never add a preset unverified). Unresolved calls never
  alarm. Worked-B4 answers within the active contest only.
- **Contest score. DONE 2026-08-28; the UI seen live over the real log
  2026-09-18.** The claimed score is an ESTIMATE from the operator's own
  seat — the sponsor's robot rescoring is the authority; that framing
  bounds the whole feature.
  Machine-readable `points=` / `mult=` rules live in the contest's own
  exch_def (unknown terms fail the parse loudly), so a contest keeps the
  rule it was operated under. Every preset rule was verified 2026-08-28
  against the sponsor's official rules, from an OK seat. SARTG carries
  points only — its multiplier wording is genuinely ambiguous and an
  ambiguity is not encoded; WAE without QTCs is a documented underestimate
  (#12). Dupes (call+band+mode) score 0 and bring nothing, CQ WW's 0-point
  own country still brings its multipliers, mult sources are namespaced (a
  YO county "CT" never collides with Portugal's prefix — caught against a
  real log). The startup backfill gives contests created before scoring
  the preset rule their ADIF id implies — never overriding a def that names
  points/mult itself. Gate: `/contest/score/*`.
- **About dialog — the family contract (2026-08-04).** Every app opens the
  same kind of About from its primary menu (last item, per the GNOME HIG),
  and its strings agree with the `.desktop` entry and the AppStream
  metainfo — one truth about the app, not three. **The version must be
  findable FROM THE UI**: a `--version` CLI flag is welcome on top, never
  instead. No acknowledgement section on purpose: nothing in this app is
  vendored. `sdr-for-linux`'s About (`src/gui.c`) is the family reference
  for the full field set.
- **Later** — DXCC/awards tracking (#10), linking imported CONTEST_ID QSOs
  to contests (#11), FT8/FT4 contests through WSJT-X (#13, deferred) and
  WAE QTC traffic (#12, **DEFERRED — Richard's call, 2026-08-04**: he has
  never taken QTCs and rides WAE CW without them; a log with no QTC lines
  is valid).

## Safety / etiquette

The logbook never transmits and never changes radio state except
operator-triggered CW/RTTY macros and keyer speed via TCI. Nothing leaves the
machine without an explicit action or an explicitly enabled sync service;
callbook/QSL credentials live in the secret service, not in config files.
Richard's global rule applies: consent before any major or irreversible step —
a destructive log operation (mass delete, re-import over existing data) always
confirms first, and every upload to an external QSL service is user-triggered
in v1 (no silent background sync).
