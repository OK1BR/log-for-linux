# Log for Linux — scope & plan

Goal: a native Linux **ham radio logbook** — the third app of the family around
**[`sdr-for-linux`](https://github.com/OK1BR/sdr-for-linux)** (SDR transceiver)
and **[`skimmer-for-linux`](https://github.com/OK1BR/skimmer-for-linux)**
(CW/RTTY/PSK skimmer), sharing their stack and architecture: a headless,
**GLib-only engine** (`src/engine/`, no GTK includes) under a GTK4/libadwaita
front-end, plain C11, meson. Successor of the deleted Rust prototype (BRlog).

Author: Richard Fakenberg, **OK1BR**. Licence: GPL-3.0-or-later.

This file says what the app **is** and **why** it is built that way. What is
open — bugs, ideas, pending live checks — lives in
[GitHub Issues](https://github.com/OK1BR/log-for-linux/issues); how each piece
was built is the git history.

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
  auto-logging, **callbook lookup** (QRZ.com/HamQTH) and **QSL sync** (LoTW
  via `tqsl`, eQSL, Club Log).
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
    ├─ callbook        (M7) libsoup · QRZ.com / HamQTH XML lookup
    └─ QSL sync        (M8) tqsl subprocess (LoTW) · eQSL · Club Log upload/download
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

- **M0 — scaffold. DONE.** Engine/app split, sqlite3 dependency, test
  harness (`log-engine-test`).
- **M1 — log store. DONE.** `src/engine/log_store.c`: schema + `PRAGMA
  user_version` migrations (a newer-than-app file is refused), WAL, CRUD,
  worked-B4 query, dup check (same call+band+mode within a time window),
  stats counters; normalized call/band/mode (upper/lower/upper),
  NULL-for-unset columns, the `extras` column for the lossless ADIF round
  trip, an explicit tx API for bulk import, list with text/band/mode filters
  + paging. Measured in-memory: 50k inserts in ~0.2 s inside one tx;
  worked-B4 + dup + list-20 in 0.1 ms. Gate: `log-store-test`.
- **M2 — ADIF import/export. DONE.** `src/engine/adif.c`, ADIF 3.1.x.
  Parser: byte-exact length-prefixed tag walk, tolerant of lowercase tags,
  CRLF, `:type` suffixes, inter-field garbage, a missing `<EOH>` (header iff
  the first non-ws char isn't `<`), 4-digit TIME_ON and a final record
  without `<EOR>`; a truncated declared length marks the record bad rather
  than eating the next one. Unmodeled fields → `extras` verbatim; BAND falls
  back to a freq→band table (2190m–23cm); bad records are counted, never
  abort; import is one tx with dup skipping (window 0 = exact-ts only).
  Writer: deterministic header (no timestamp — same log ⇒ byte-identical
  file), fixed field order, locale-safe trimmed numbers, oldest-first.
  Gate: `log-adif-test` (quirks corpus, UTF-8 names, multiline comments,
  dedup accounting, export(import(x)) byte-stability).
- **M3 — UI v1: the usable logbook. DONE — manual checklist signed off by
  Richard 2026-07-21; the app is v1 for daily use.** `src/app/win.c`
  (+ `qso_row.c` GObject row wrapper): entry row with UTC clock, live
  worked-B4 hint (green "New call" / yellow B4 counts per band/band+mode
  with last-worked date), RST defaults per mode (phone 59 / CW-like 599 /
  FT8·FT4 blank; never stomping a hand-edited report), freq → band dropdown
  auto-sync via the M2 band table (the dropdown lists every ADIF band the
  engine knows), Enter anywhere logs; a 5-min same call+band+mode duplicate
  asks first (default Cancel). The QSO table lives in the main window under
  the macro strip — virtualized GtkColumnView (newest first), search with
  250 ms debounce, inline cell edit + right-click delete (confirm); the
  footer holds UTC + TCI status. ADIF import/export via GtkFileDialog +
  GFile I/O with a result toast; import dedup is exact-timestamp only;
  QSO+calls counters in the window subtitle. A store open failure surfaces
  a dialog instead of aborting.
- **M4 — TCI integration. DONE, in live contest use since EUHFC
  2026-08-01.** `src/engine/tci_client.c` (libwebsockets, text plane only —
  no IQ) connects to `sdr-for-linux` (`ws://127.0.0.1:40001`): handshake to
  `ready;`, tracks `vfo`/`modulation`/`device`/`protocol`, maps the radio
  mode into the logbook dropdown (cw→CW, usb/lsb→SSB, digu/digl→FT8,
  rtty→RTTY, …). UI: background connect + 5 s reconnect, status line next
  to the UTC clock, auto-prefill of MHz/band/mode from the radio. TCI
  host/port and the station callsign live in Preferences → GKeyFile
  `~/.config/log-for-linux/settings.ini` (`src/app/settings.c`, family
  house style: `AdwPreferencesDialog`, save on dialog close; a host/port
  change reconnects immediately). Table-driven QSY was tried and dropped
  (2026-07-21): not useful enough for a toolbar control, and a click on a
  cell is reserved for inline edit; `tune()` stays in the engine API. The
  logbook never changes radio state except operator-triggered CW/RTTY
  macros and keyer speed. Gate: `log-tci-test` (mock TCI server, skimmer
  house pattern).
  **Keyer speed:** Page Up/Down nudge WPM by 1 from anywhere in the entry
  window (contest-logger style, focus stays in Call), sent as TCI
  `cw_macros_speed:<wpm>` and clamped 5–60 to match sdr-for-linux; the
  radio's echo (handshake, own controls, our set) is the authority and
  shows in the TCI status line. Falls through to default scrolling while a
  cell is edited or when no speed is known yet.
  **Spot-click prefill:** clicking a skimmer spot on the sdr-for-linux
  panadapter makes it broadcast `rx_clicked_on_spot` / `clicked_on_spot`;
  the logbook takes the callsign into the Call entry (upper-cased,
  sanity-checked, receiver 0 only), presents its window and focuses Call
  with the text selected (double-click feel — Tab/Enter keeps, typing
  replaces), and the worked-B4 / dup check runs on it as if typed. Spots
  themselves stay one-way client→server in TCI, so the logbook never sees
  the spot list — this is the tiny prefill the no-cluster-window rule
  leaves room for, not a spot window. An already-typed call is never
  overwritten and an open cell editor is never interrupted, which also
  makes the two spellings of one click idempotent. The prefill stays a
  prefill until the operator types anywhere into the QSO row — Call, RST,
  exchange, Sent, Name or Comment. Until then clicking another spot
  replaces it, and tuning more than 200 Hz off the spot (that call is no
  longer on frequency) resets the WHOLE entry row: RST to the mode default,
  Sent back to the serial/exchange prefill, focus in Call — with an open
  cell editor the row still resets but focus stays in the cell — so
  leftovers of a QSO that never happened cannot leak into the next one.
  Once typed into, the row is the operator's: QSY never deletes it and a
  spot click never overwrites it. The New-call / worked-B4 / DUP verdict
  line is 1.3em bold with family colors: New = #30C060 (the skimmer's own
  spot green on the panadapter), DUP = saturated #ED333B.
- **M5 — macros v2 (contest-style messaging). DONE; in live contest use
  since EUHFC 2026-08-01** (where `{NR}`/`{EXCH}`, cut numbers, the word
  gap and Ctrl+K were verified live). The F-key strip (F1–F8 + Esc stop) is
  a messaging layer inspired by N1MM+, not a clone; expansion lives in the
  engine (`logfl_macro_expand`). Gate: `log-macro-test` (expand, bank
  defaults/edit, ESM transitions).
  1. **Editable macros** — right-click an F-key → caption + template
     dialog, plus a full editor in Preferences → Messaging (both banks
     behind a Run/S&P switcher, one expander per key, edits apply live and
     save on dialog close). Persisted in `settings.ini` groups `macros_run`
     / `macros_snp`. Tokens: `{MYCALL}` `{CALL}` `{RST}` `!`, `{NR}` (sent
     serial, "001" form) and `{EXCH}` (static sent exchange), both fed from
     the Sent entry; empty tokens collapse, so nothing changes outside
     contests. The default F2 is `{CALL} {RST} {NR} {EXCH}` (S&P: `{RST}
     {NR} {EXCH}`) and S&P F7 asks `NR?` in the spirit of F6's `AGN?`.
  2. **Run vs S&P** — two 8-key banks with distinct defaults; Run/S&P
     toggle on the entry window; active bank persisted as `contest.bank`.
  3. **ESM — Enter sends message** — optional (`contest.esm` /
     Preferences), off by default so daily logging keeps Enter = log.
     Enter advances READY→EXCH→LOG→TU (empty call → CQ; after log with a
     cleared call still TU). The **Log QSO** button always logs.
  4. **Cut numbers** (opt-in, Preferences → Messaging → CW keyer):
     `logfl_macro_cut_apply` rewrites digits per `[cw] cut_map` pairs, one
     switch per standard substitution (0T 1A 2U 3V 5E 8D 9N) — applied at
     send time to RST/serial/all-digit exchange only, never callsigns, and
     only in CW; the log keeps real digits.
  5. **Queued-message word gap:** text goes out with a leading space,
     SDC-style — sdr-for-linux's generator inserts the inter-message gap
     only when the following text asks for it (a trailing space does
     nothing; skipped on an idle keyer, so overs never start with dead
     air).
  6. **Ctrl+K free text** (N1MM style): small non-modal window, Enter keys
     the line and clears for the next, Esc stops the keyer and closes, the
     Stop button just stops.
  7. **SSB "wav" / DVK — OUT OF SCOPE**: text keying via TCI only.
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

  Reference sequence (the whole family): the skimmer spots an RTTY CQ (pair
  centre) → operator clicks → sdr-for-linux tunes dial = pair centre and
  relays the click → the logbook prefills Call, the dropdown reads RTTY →
  F2 → `{CALL} 599 {NR}` expands → `rtty_macros:0, DL1ABC 599 001;` → the
  radio keys direct FSK → mark tail → RX. Esc → both stops → the radio
  ramps down within one block.
- **M6 — WSJT-X UDP. CODE DONE (offline gate green); the live check with a
  real FT8 QSO is #6.** `src/engine/wsjtx_udp.c` (QDataStream BE,
  utf8=QByteArray, schema 2/3): GSocket server on the GLib main loop
  (default `127.0.0.1:2237`), heartbeat reply; `QSO Logged` is decoded into
  the store (exact-ts dup skip, toast, table reload), `Status` is answered
  with worked-B4 via `Highlight Callsign` (green = new, yellow = worked).
  Preferences → WSJT-X (enable + port); footer status line. Gate:
  `log-udp-test` (synthetic QSO Logged/Status round-trip, store insert,
  loopback server).
- **Dup lookup service for skimmer-for-linux. DONE, live-verified in EUHFC
  2026-08-01.** Read-only UDP line protocol on `127.0.0.1:2238`, always on
  with the app: `DUP? <call> <freq_hz> <mode>` → `NEW|B4|DUP|INV <call>`
  back to the sender. DUP = call+band+mode already in the ACTIVE contest
  (band derived from the frequency), B4 = worked any time, NEW = not in
  the log, INV = not a valid contest QSO (strongest skip; the skimmer
  paints it grey); malformed requests get silence, so the skimmer treats a
  timeout as unknown and spotting survives the logbook being closed.
  **Push:** the service remembers peers with a valid `DUP?` in the last
  10 min (8 slots) and any verdict-changing mutation — manual log, WSJT-X
  QSO, delete, cell edit (old + new identity) — sends them the same answer
  datagram unsolicited, so the skimmer regrays the live panadapter label at
  once instead of on its ≤180 s re-announce. The logbook deliberately never
  writes TCI `spot:` — the label color is the skimmer's to own (two writers
  would race). Engine transport in `src/engine/dup_srv.c` (the verdict
  callback in the app owns store + contest context); both UDP handlers
  (dup + WSJT-X) receive regardless of the wake condition — Linux delivers
  async ICMP errors on unconnected UDP sockets and a G_IO_ERR-only wake
  must be drained by the recv or the main loop spins. Gate:
  `log-dupq-test`. The skimmer side (querying, caching, coloring) lives in
  skimmer-for-linux (its SCOPE.md).
- **Hand-typed text is upper case as it is typed (Richard 2026-08-14;
  exchange added after YO DX HF 2026-08-22).** Call, the exchange fields
  and Sent — in the entry row and in the inline cell editor of a saved
  QSO — show capitals while being typed, whatever Caps Lock happens to be
  doing: the operator never looks at the keyboard mid-QSO and must see what
  will really go out. The store normalizes anyway; this closes the
  lower-case path the operator sees. `entry_force_upper` in win.c is an
  insert-text filter on the entry's GtkText delegate (upcased re-insert +
  stop emission, so cursor/selection stay put; GTK4 does not forward
  insert-text to the GtkEntry wrapper).
- **Edit saved QSO. DONE (decided with Richard 2026-07-21).** Correcting a
  logged QSO is first-class and inline-only; the entry strip stays for
  **new** QSOs (no pencil / load-into-entry). No row selection
  (GtkNoSelection): the theme's row hover marks the edit target and a
  single click on a cell opens an inline `GtkEntry` — single-click beats
  double-click, the hover then reads as "click to edit", not as a pointless
  flash. Fields: UTC, call, band, MHz, mode[/submode], RST (`sent/rcvd`),
  Sent/Rcvd exchange, name, comment; Enter commits (the label updates
  immediately) — everything else discards: Esc, a click anywhere outside
  the cell, focus-out, scroll-away. Delete is right-click on the row →
  confirm dialog (names the QSO, targets the clicked row; no intermediate
  context menu). No table QSY. The engine path is `logfl_store_get` +
  `logfl_store_update`, so extras, QSL flags, grid/QTH/power and station
  fields stay intact when a single cell is changed.
- **Freq is always stored exactly.** `freq REAL` is filled from: typed MHz
  → live TCI VFO at log time → band mid-point fallback; a band change seeds
  MHz when empty; TCI overwrites with the real VFO.
- **M7 — callbook lookup.** QRZ.com XML (subscriber) / HamQTH (free) —
  name/QTH/grid auto-fill on callsign entry, on-disk cache, credentials in the
  keyring, never in config files.
  Gate: `log-callbook-test` against canned XML; live smoke against both APIs.
- **M8 — QSL sync.** LoTW: sign+upload via `tqsl` CLI, pull confirmations
  (`lotwreport.adi`) and mark QSLs; eQSL upload + inbox; Club Log upload.
  Per-QSO sent/confirmed state per service, retry-safe (idempotent re-upload).
  Gate: `log-qsl-test` over mocked endpoints; live check with a small batch.
- **M9 — contest management. DONE (decided 2026-07-27 with Richard, pulled
  ahead of M7/M8).** Contests are first-class log sections: create/delete/
  switch, each contest with its own exchange template.
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

  Engine: schema v2 (contest table + contest_ref/stx/srx/stx_string/
  srx_string), contest CRUD, per-contest serial (`max(stx)+1`),
  whole-contest dup check (call+band+mode), list scoping (all/main/contest),
  `contest.c` exchange templates (GKeyFile serialization) +
  `logfl_exch_apply` routing; ADIF: STX/SRX[_STRING] modeled as columns,
  CONTEST_ID written from the linked contest's `adif_id`; an imported
  CONTEST_ID stays verbatim in extras (no auto-created contests —
  deliberate). Gate: `log-contest-test`.
  UI: header switcher (Main log / contests / New contest… / Manage…),
  new-contest dialog (name, preset, editable fields, my exchange, ADIF id),
  the entry row grows template fields + next-serial display, per-contest
  dup warning, exchange column in the table, active contest persisted in
  settings.ini (`contest.active`). WSJT-X auto-logged QSOs always land in
  the main log (FT8 contest support would come later, deliberately).
- **Cabrillo export. DONE.** `src/engine/cabrillo.c`: WWROF v3 shape
  verified against wwrof.org and the official EUHFC sample (log.s5cc.eu):
  tag header (empty fields omitted, CREATED-BY stamped), chronological QSO
  lines — kHz from the exact QRG, generic band edge without one (per EUHFC
  rules note), VHF+ band designators; modes CW/PH/FM/RY/DG; missing RSTs
  default 599/59; sent serial zero-padded + exchange text. Gate:
  `log-cabrillo-test`. UI: menu → Export Cabrillo… (exports the ACTIVE
  contest), header dialog with CATEGORY-* as spec-value dropdowns (incl.
  optional ASSISTED, "—" = omit). Operator, Power, Transmitter and Assisted
  are persisted in settings.ini `[cabrillo]`; **CATEGORY-MODE and
  CATEGORY-BAND are derived from the contest's own QSOs**
  (`logfl_cabrillo_categories_from_log()` — a remembered RTTY from SARTG
  once mislabelled the all-CW YO DX entry): mode families collapse to
  MIXED, bands to ALL, the remembered value is only a fallback for a
  contest with no QSOs yet, and both rows stay editable because the log
  proves what was worked, not which category was entered. CONTEST is
  prefilled via a known-map (ADIF `EU-HF` → Cabrillo `EUHFC`); the file
  defaults to `<call>.log`; CLAIMED-SCORE is prefilled from the score
  estimate below (editable, never persisted).
- **cty resolver + contest validity rules. DONE (triggered mid-WAE
  2026-08-08: an EU station gave Richard no serial — WAE scores EU↔non-EU
  only, and the log had no idea).** `src/engine/cty.c`: parser for the AD1C
  cty.dat (MIT; snapshot + license note in `data/`, installed to the app
  data dir, user-replaceable) — exact `=CALL` entries, longest prefix,
  zone/continent overrides, portable-call heuristics; gate `log-cty-test`
  runs format corners plus the real snapshot. Presets carry a validity rule
  in exch_def (`counts=all|eu-dx|eu-only`, `zero_own_country`), each
  verified against the sponsor's official rules (sources in preset
  comments; house rule: never add a preset unverified): WAE=eu-dx,
  EUHFC=eu-only, CQ WW=zero own country, others all-valid.
  `logfl_contest_qso_validity` judges my-side × their-side; unresolved
  calls never alarm. UI: the B4 line leads with "No contest QSO — EU
  station (Finland)" (error) / "0 pts — own country" (warning), and a fresh
  call shows "New call from Czech Republic". Worked-B4 answers within the
  active contest only.
- **Contest score. DONE 2026-08-28; the UI seen live over the real log
  2026-09-18.** The
  claimed score is an ESTIMATE from the operator's own seat — the sponsor's
  robot rescoring is the authority; that framing bounds the whole feature.
  Machine-readable `points=` / `mult=` rules live in the contest's own
  exch_def (terms judged via the cty resolver; unknown terms fail the parse
  loudly), so a contest keeps the rule it was operated under. Every preset
  rule was verified 2026-08-28 against the sponsor's official rules, from
  an OK seat: CQ WW 0/1/3 + zones+countries, WPX 1, 1/2, 3/6 + prefixes
  once per contest, IARU 1/1/3/5 with HQ text exchanges + zones+HQ mults,
  OK/OM DX 2/3/5 + counties+countries, EUHFC 1 + years per band, WAE 1 +
  call-area mults weighted ×4/×3/×2 (QTCs deferred — a documented
  underestimate), CVA 2/3/4 + PY states+countries, YO DX 8/1/2/4 +
  counties+countries. SARTG carries points only — its multiplier wording is
  genuinely ambiguous and an ambiguity is not encoded. The engine scorer
  (`logfl_contest_score`) walks the contest chronologically: dupes
  (call+band+mode) score 0 and bring nothing, validity applies (CQ WW's
  0-point own country still brings its multipliers), mult sources are
  namespaced (a YO county "CT" never collides with Portugal's prefix —
  caught against a real log). UI: live "pts × mult = total" in the contest
  subtitle, Pts/Mult table columns (computed, not editable, shown only
  while a rule runs — Mult shows what the QSO brought first), CLAIMED-SCORE
  prefilled in the Cabrillo dialog. The startup backfill gives contests
  created before scoring the preset rule their ADIF id implies — never
  overriding a def that names points/mult itself. Gate: `/contest/score/*`.
- **About dialog — the family contract (2026-08-04).** Every app opens the
  same kind of About from its primary menu (last item, per the GNOME HIG),
  and its strings agree with the `.desktop` entry and the AppStream
  metainfo — one truth about the app, not three. **The version must be
  findable FROM THE UI**: a `--version` CLI flag is welcome on top, never
  instead. No acknowledgement section on purpose: nothing in this app is
  vendored. `sdr-for-linux`'s About (`src/gui.c`) is the family reference
  for the full field set.
- **Later** — DXCC/awards tracking (worked/confirmed matrices per band/mode
  on top of the cty resolver), WAE QTC traffic (next bullet), linking
  imported CONTEST_ID QSOs to contests, and — only if ever revisited — the
  skimmer cluster client.
- **WAE QTC traffic. DEFERRED (Richard's call, 2026-08-04).** A QTC is the
  report of a prior contest QSO back to a European station (DARC WAE rules
  §7): DX stations transmit numbered series ("QTC 3/7") of time/call/serial
  triples, one point per correctly copied QTC for both sides, up to 10 per
  station pair — roughly doubling a serious score. Richard has never taken
  QTCs and rides WAE CW without them; a log with no QTC lines is valid. If
  ever built: a receive window shaped for CW copy speed (series header plus
  time/call/serial rows), storage carrying the QTC transmission's own time
  and band (the rules require logging those), the 10-per-station quota, and
  Cabrillo `QTC:` lines (QRG, MODE, DATE, TIME, CALL-RX, QTC-GRP, CALL-TX,
  TIME-QSO, CALL-QSO, NR-QSO). Build and practice it calmly before a WAE
  SSB/RTTY edition, never mid-contest.

## Safety / etiquette

The logbook never transmits and never changes radio state except
operator-triggered CW/RTTY macros and keyer speed via TCI. Nothing leaves the
machine without an explicit action or an explicitly enabled sync service;
callbook/QSL credentials live in the secret service, not in config files.
Richard's global rule applies: consent before any major or irreversible step —
a destructive log operation (mass delete, re-import over existing data) always
confirms first, and every upload to an external QSL service is user-triggered
in v1 (no silent background sync).
