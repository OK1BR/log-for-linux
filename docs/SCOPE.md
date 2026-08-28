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
- **M3 — UI v1: the usable logbook. IMPLEMENTED 2026-07-21 — manual
  checklist signed off by Richard (`docs/M3-CHECKLIST.md`); the app is v1
  for daily use.** Entry row (call/RST/band/mode/…,
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
  QSO table lives **in the main window under the macro strip** —
  virtualized GtkColumnView (newest first), search with 250 ms debounce,
  inline cell edit + right-click delete (confirm); footer holds UTC + TCI
  status.
  ADIF import/export via GtkFileDialog + GFile I/O with a result toast;
  import dedup is exact-timestamp only; QSO+calls counters in both window
  subtitles. Store open failure surfaces a dialog instead of aborting.
- **M4 — TCI integration. CODE LANDED 2026-07-21 (offline gate green).**
  Connect to `sdr-for-linux` (`ws://127.0.0.1:40001`), entry row pre-fills
  freq/mode from the live VFO. Table-driven QSY was tried and dropped
  (2026-07-21): not useful enough for a toolbar control; a click on a cell
  is reserved for inline edit.
  Gate: `log-tci-test` — mock TCI server (skimmer house pattern); live check
  against the real radio when available.
  Done as `src/engine/tci_client.c` (libwebsockets, text plane only — no IQ):
  handshake to `ready;`, track `vfo`/`modulation`/`device`/`protocol`,
  `tune()` kept in the engine API for possible later use, mode map into the
  logbook dropdown (cw→CW, usb/lsb→SSB, digu/digl→FT8, …). UI (`win.c`):
  background connect + 5 s reconnect, status line next to the UTC clock,
  auto-prefill of MHz/band/mode from the radio. TCI host/port (and station
  callsign) live in Preferences → GKeyFile
  `~/.config/log-for-linux/settings.ini` (`src/app/settings.c`, skimmer/sdr
  house style: `AdwPreferencesDialog`, save on dialog close; host/port change
  reconnects immediately). Entry window has an N1MM-inspired **macro bar v1**
  (F1–F8 + Esc stop): fixed CQ/EXCH/TU/MY/HIS/AGN/QRZ/STOP with `{MYCALL}`
  `{CALL}` `{RST}` expansion; CW text via TCI `cw_macros` to sdr-for-linux
  when connected. Full editable macros / Run·S&P / ESM → **M5**. The logbook
  never changes radio state except operator-triggered CW macros and keyer
  speed. **Keyer speed (2026-08-01):** Page Up/Down nudge WPM by 1 from
  anywhere in the entry window (contest-logger style, focus stays in Call),
  sent as TCI `cw_macros_speed:<wpm>` and clamped 5–60 to match
  sdr-for-linux; the radio's echo (handshake, own controls, our set) is the
  authority and shows in the TCI status line. Falls through to default
  scrolling while a cell is edited or when no speed is known yet.
  **Spot-click prefill (2026-08-01):** clicking a skimmer spot on the
  sdr-for-linux panadapter makes it broadcast `rx_clicked_on_spot` /
  `clicked_on_spot`; the logbook takes the callsign into the Call entry
  (upper-cased, sanity-checked, receiver 0 only) and the worked-B4 / dup
  check runs on it as if typed. Spots themselves stay one-way client→server
  in TCI, so the logbook never sees the spot list — this is the tiny prefill
  the SCOPE's no-cluster-window rule leaves room for, not a spot window. An
  already-typed call is never overwritten and an open cell editor is never
  interrupted, which also makes the two spellings of one click idempotent.
  The prefill is tracked as such until the operator types anywhere into the
  QSO row — Call, RST, exchange, Sent, Name or Comment (LOG-2, 2026-08-28;
  before that only Call counted, so a spot click could pair a new call with
  the previous station's half-copied exchange): clicking another spot
  replaces it, and tuning more than 200 Hz off the spot clears it (that call
  is no longer on frequency). Once typed into, the row is the operator's —
  QSY never deletes it and a spot click never overwrites it.
  **Live-EUHFC hardening (2026-08-01, tuned mid-contest):** the click also
  presents the logbook window and focuses Call with the text selected
  (double-click feel — Tab/Enter keeps, typing replaces); QSY off an
  untouched spot resets the WHOLE entry row via `entry_reset_defaults`
  (RST to the mode default, Sent back to the serial/exchange prefill,
  focus in Call — with an open cell editor the row still resets but focus
  stays in the cell; LOG-2, 2026-08-28, before that only Call cleared
  there) so leftovers of a QSO that never happened cannot leak into the
  next one. The New-call / worked-B4 / DUP verdict line is
  1.3em bold with family colors: New = #30C060 (the skimmer's own spot
  green on the panadapter), DUP = saturated #ED333B.
- **M5 — macros v2 (contest-style messaging). IMPLEMENTED 2026-07-21
  (offline gate green).** Grew the F-key strip into a messaging layer
  (inspired by N1MM+, not a clone). Done:
  1. **Editable macros** — right-click F-key → caption + CW template dialog;
     persist in `settings.ini` groups `macros_run` / `macros_snp`; tokens
     `{MYCALL}` `{CALL}` `{RST}` and `!` via engine `logfl_macro_expand`.
     Since 2026-07-21 also a full editor in Preferences → Messaging: both
     banks behind a Run/S&P switcher, one expander per key, edits apply
     live and save on dialog close.
  2. **Run vs S&P** — two 8-key banks with distinct defaults; Run/S&P
     toggles on the entry window; active bank persisted as `contest.bank`.
  3. **ESM — Enter sends message** — optional (`contest.esm` / Preferences);
     Enter advances READY→EXCH→LOG→TU (empty call → CQ; after log with
     cleared call still TU). **Log QSO** button always logs. Off by default
     so daily logging keeps Enter = log.
  4. **SSB “wav” / DVK — OUT OF SCOPE** (unchanged): CW text via TCI only.
  Gate: `log-macro-test` — expand, bank defaults/edit, ESM transitions.
  Live CW smoke against sdr-for-linux when available (not formal signed gate).
  **Contest-day round (2026-08-01, all live-verified in EUHFC):**
  `{NR}`/`{EXCH}` tokens (sent serial / static exchange, fed from the Sent
  entry) — the default F2 is `{CALL} {RST} {NR} {EXCH}` (S&P: `{RST} {NR}
  {EXCH}`; empty tokens collapse, so nothing changes outside contests) and
  S&P F7 asks `NR?` in the spirit of F6's `AGN?`. **Cut numbers** (opt-in,
  Preferences → Messaging → CW keyer): `logfl_macro_cut_apply` rewrites
  digits per `[cw] cut_map` pairs, one switch per standard substitution
  (0T 1A 2U 3V 5E 8D 9N) — applied at send time to RST/serial/all-digit
  exchange only, never callsigns; the log keeps real digits. **Queued-
  message word gap:** CW text goes out with a leading space, SDC-style —
  sdr-for-linux's generator inserts the inter-message gap only when the
  following text asks for it (a trailing space does nothing; skipped on an
  idle keyer, so overs never start with dead air). **Ctrl+K free CW text**
  (N1MM style): small non-modal window, Enter keys the line and clears for
  the next, Esc stops the keyer and closes, Stop button just stops.
- **M6 — WSJT-X UDP. IMPLEMENTED 2026-07-21 (offline gate green).**
  UDP server (default `127.0.0.1:2237`): decode `QSO Logged` into the store,
  on `Status` answer worked-B4 via `Highlight Callsign` (green = new, yellow
  = worked). Preferences → WSJT-X (enable + port); footer status line.
  Done as `src/engine/wsjtx_udp.c` (QDataStream BE, utf8=QByteArray, schema
  2/3): pack/parse helpers, GSocket server on the GLib main loop, heartbeat
  reply. UI wires auto-log (exact-ts dup skip) + toast + table reload.
  Gate: `log-udp-test` — synthetic QSO Logged/Status round-trip, store insert,
  loopback server. Live check: one real FT8 QSO from WSJT-X (not formal gate).
- **Dup lookup service for skimmer-for-linux. IMPLEMENTED 2026-08-01
  (gate green, live-verified in EUHFC).** Read-only UDP line protocol on
  `127.0.0.1:2238`, always on with the app: `DUP? <call> <freq_hz> <mode>`
  → `NEW|B4|DUP <call>` back to the sender. DUP = call+band+mode already
  in the ACTIVE contest (band derived from the frequency), B4 = worked any
  time, NEW = not in the log; malformed requests get silence, so the
  skimmer treats a timeout as unknown and spotting survives the logbook
  being closed. **Push:** the service remembers peers with a valid `DUP?`
  in the last 10 min (8 slots) and any verdict-changing mutation — manual
  log, WSJT-X QSO, delete, cell edit (old + new identity) — sends them the
  same answer datagram unsolicited, so the skimmer regrays the live
  panadapter label at once instead of on its ≤180 s re-announce. The
  logbook deliberately never writes TCI `spot:` — the label color is the
  skimmer's to own (two writers would race). Engine transport in
  `src/engine/dup_srv.c` (the verdict callback in the app owns store +
  contest context); both UDP handlers (dup + WSJT-X) receive regardless
  of the wake condition — Linux delivers async ICMP errors on unconnected
  UDP sockets and a G_IO_ERR-only wake must be drained by the recv or the
  main loop spins. Gate: `log-dupq-test` — parser, loopback round trip,
  push to multiple peers, no-peer no-op. The skimmer side (querying,
  caching, coloring) lives in skimmer-for-linux (its SCOPE.md).
- **Hand-typed call is always upper case (Richard 2026-08-14, DONE
  2026-08-15).** A callsign typed by hand into the Call entry shows in
  capitals while it is being typed, whatever Caps Lock happens to be doing —
  the operator never has to look at the keyboard mid-QSO. Forced in the
  entry itself, not only at log time: the store already normalizes `call`
  to upper and the spot-click prefill upper-cases, this closed the one
  lower-case path the operator sees. Implemented as an insert-text filter
  on the entry's GtkText delegate (upcased re-insert + stop emission, so
  cursor/selection stay put; GTK4 does not forward insert-text to the
  GtkEntry wrapper) — `entry_force_upper` in win.c, applied to the Call
  entry and to the Call column of the inline cell editor of a saved QSO.
  GUI-only behavior, not gate-reachable; awaits the live check.
- **M7 — callbook lookup.** QRZ.com XML (subscriber) / HamQTH (free) —
  name/QTH/grid auto-fill on callsign entry, on-disk cache, credentials in the
  keyring, never in config files.
  Gate: `log-callbook-test` against canned XML; live smoke against both APIs.
- **M8 — QSL sync.** LoTW: sign+upload via `tqsl` CLI, pull confirmations
  (`lotwreport.adi`) and mark QSLs; eQSL upload + inbox; Club Log upload.
  Per-QSO sent/confirmed state per service, retry-safe (idempotent re-upload).
  Gate: `log-qsl-test` over mocked endpoints; live check with a small batch.
- **Edit saved QSO. IMPLEMENTED 2026-07-21; inline-only 2026-07-21.**
  Decided 2026-07-21 (Richard): correcting a logged QSO is first-class;
  entry strip stays for **new** QSOs only (no pencil / load-into-entry).
  UI: no row selection (GtkNoSelection); the theme's row hover highlight
  marks the edit target and a single click on a cell opens an inline
  `GtkEntry` (2026-07-21, Richard: single-click beats double-click — the
  hover then reads as "click to edit", not as a pointless flash).
  Fields: UTC, call, band, MHz, mode[/submode], RST
  (`sent/rcvd`), name, comment; Enter commits (label updates immediately) —
  everything else discards: Esc, a click anywhere outside the cell,
  focus-out, scroll-away (2026-07-21, Richard). Delete is right-click on
  the row → confirm dialog (names the QSO, targets the clicked row; no
  intermediate context menu). No table QSY. Engine
  path is `logfl_store_get` + `logfl_store_update` so extras, QSL flags,
  grid/QTH/power and station fields stay intact when a single cell is
  changed. Also covers fixing early QSOs saved with `freq` NULL (band only)
  before the MHz prefill/TCI fallback landed.
- **Freq must be exact in SQL (caught 2026-07-21).** Schema always had
  `freq REAL`; the UI used to leave the MHz entry empty (placeholder only),
  so `bind_qso` stored NULL and only `band` survived. Fix direction: typed
  MHz → live TCI VFO at log time → band mid-point fallback; band change
  seeds MHz when empty; TCI still overwrites with the real VFO. Existing
  NULL rows need edit-QSO or a one-shot repair later.
- **M9 — contest management. Decided 2026-07-27 (Richard), pulled ahead of
  M7/M8.** Contests as first-class log sections: create/delete/switch, each
  contest with its own exchange template.
  Decisions (2026-07-27):
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
     creation — editing presets never rewrites history. Presets: CQ WW,
     CQ WPX, IARU HF, OK/OM DX, Custom; more added over time as needed
     (national/event contests).
  4. **Main log view = non-contest QSOs only** (`contest_ref` NULL); a
     contest's QSOs are visible when switched into it. Worked-B4 and stats
     stay global. ADIF export always exports everything by default.
  Engine (landed 2026-07-27): schema v2 migration (contest table +
  contest_ref/stx/srx/stx_string/srx_string), contest CRUD, per-contest
  serial (`max(stx)+1`), whole-contest dup check (call+band+mode), list
  scoping (all/main/contest), `contest.c` exchange templates (GKeyFile
  serialization) + `logfl_exch_apply` routing, ADIF: STX/SRX[_STRING]
  modeled as columns, CONTEST_ID written from the linked contest's
  `adif_id`; imported CONTEST_ID stays verbatim in extras (no auto-created
  contests — deliberate). Macros grew {NR} (sent serial, "001" form) and
  {EXCH} (static sent exchange).
  Gate: `log-contest-test` — definitions round-trip, presets, routing,
  v1→v2 migration, CRUD, serial/dup/scoping, delete semantics, ADIF
  round trip.
  UI: header switcher (Main log / contests / New contest… / Manage…),
  new-contest dialog (name, preset, editable fields, my exchange, ADIF id),
  entry row grows template fields + next-serial display, per-contest dup
  warning, exchange column in the table, active contest persisted in
  settings.ini (`contest.active`). WSJT-X auto-logged QSOs always land in
  the main log (FT8 contest support would come later, deliberately).
- **Cabrillo export. IMPLEMENTED 2026-07-28** (pulled ahead for EUHFC —
  submission is Cabrillo-only within 48 h). `src/engine/cabrillo.c`:
  WWROF v3 shape verified against wwrof.org and the official EUHFC sample
  (log.s5cc.eu): tag header (empty fields omitted, CREATED-BY stamped),
  chronological QSO lines — kHz from the exact QRG, generic band edge
  without one (per EUHFC rules note), VHF+ band designators; modes
  CW/PH/FM/RY/DG; missing RSTs default 599/59; sent serial zero-padded +
  exchange text. Gate: `log-cabrillo-test`. UI: menu → Export Cabrillo…
  (exports the ACTIVE contest), header dialog with CATEGORY-* as
  spec-value dropdowns (incl. optional ASSISTED, "—" = omit). Operator,
  Power, Transmitter and Assisted are persisted in settings.ini
  `[cabrillo]`; **CATEGORY-MODE and CATEGORY-BAND are derived from the
  contest's own QSOs** (`logfl_cabrillo_categories_from_log()`, LOG-4
  2026-08-23, after a remembered RTTY from SARTG mislabelled the all-CW
  YO DX entry) — mode families collapsing to MIXED, bands to ALL, the
  remembered value only a fallback for a contest with no QSOs yet, and
  both rows staying editable because the log proves what was worked, not
  which category was entered. CONTEST prefilled via a known-map (ADIF
  `EU-HF` → Cabrillo `EUHFC`); file defaults to `<call>.log`. Claimed
  score / scoring itself stays out (Later).
- **A GNOME-correct About dialog. IMPLEMENTED 2026-08-04** (family-wide
  contract written down that day; closed here the same day, Richard checked
  the dialog visually). The contract: every app opens the same kind of
  About from its primary menu (last item, per the GNOME HIG), and its
  strings agree with the `.desktop` entry and the AppStream metainfo — one
  truth about the app, not three. **The version must be findable FROM THE
  UI** (Richard, 2026-08-04): a `--version` CLI flag is welcome on top,
  never instead — the About dialog is where someone launched from the app
  grid sees it. Done in `act_about` (`src/app/win.c`) by adding the four
  missing setters to the existing `AdwAboutDialog`: `application_icon`
  (= the GApplication id `cz.ok1br.log_for_linux`; without it the dialog
  showed a generic icon), `comments` — "Ham radio logbook", the same
  one-liner the metainfo `<summary>` and the `.desktop` Comment carry —
  `copyright` "© 2026 Richard Fakenberg, OK1BR", and `issue_url` (the
  metainfo bugtracker URL). No acknowledgement section on purpose: nothing
  in this app is vendored (sqlite3 is a plain dependency, the TCI client is
  family code). `sdr-for-linux`'s About (`src/gui.c`) remains the family
  reference for the full field set.
- **The Website field in the repo header. DONE 2026-08-04** (family-wide
  note from the same day). `gh repo edit OK1BR/log-for-linux --homepage
  https://rifak.cz`, verified back via `gh repo view` — the GitHub sidebar
  now links where the README already pointed.
- **cty resolver + contest validity rules. IMPLEMENTED 2026-08-08**
  (triggered mid-WAE: an EU station gave Richard no serial — WAE scores
  EU↔non-EU only, and the log had no idea). `src/engine/cty.c`: parser for
  the AD1C cty.dat (MIT; snapshot + license note in `data/`, installed to
  the app data dir, user-replaceable) — exact `=CALL` entries, longest
  prefix, zone/continent overrides, portable-call heuristics; gate
  `log-cty-test` runs format corners plus the real snapshot. Presets carry
  a validity rule in exch_def (`counts=all|eu-dx|eu-only`,
  `zero_own_country`), each verified 2026-08-08 against the sponsor's
  official rules (sources in preset comments; house rule in auto-memory:
  never add a preset unverified): WAE=eu-dx, EUHFC=eu-only, CQ WW=zero own
  country, others all-valid. `logfl_contest_qso_validity` judges my-side ×
  their-side; unresolved calls never alarm. UI: the B4 line leads with
  "No contest QSO — EU station (Finland)" (error) / "0 pts — own country"
  (warning), and a fresh call shows "New call from Czech Republic". The
  dup service grew the INV verdict (strongest skip; skimmer paints grey —
  its own mapping change lives in skimmer-for-linux). Same day the B4/dup
  scope bug fell: worked-B4 answers within the active contest only
  (e703409).
- **Later** — DXCC/awards tracking (worked/confirmed matrices per band/mode
  on top of the cty resolver), WAE QTC traffic (next bullet), linking
  imported CONTEST_ID QSOs to contests, and — only if ever revisited — the
  skimmer cluster client.
- **Contest score calculation. IDEA (Richard, 2026-08-08).** Live claimed
  score in the contest UI + Cabrillo CLAIMED-SCORE: per-contest QSO points
  (the per-preset rules verified 2026-08-08 from official sources — WAE
  weighted band multipliers ×4/×3/×2, CQ WW 0-point own country, OK/OM DX
  2/3/5+10, IARU 1/3/5, CVA 2/3/4, SARTG 5/10/15, EUHFC 1) and multiplier
  tracking per band. Builds directly on the cty.dat resolver and the
  per-preset validity rules (2026-08-08); the scoring tables are already
  researched and sourced in the preset comments.
- **WAE QTC traffic. DEFERRED (Richard's call, 2026-08-04).** A QTC is the
  report of a prior contest QSO back to a European station (DARC WAE rules
  §7): DX stations transmit numbered series ("QTC 3/7") of time/call/serial
  triples, one point per correctly copied QTC for both sides, up to 10 per
  station pair — roughly doubling a serious score. Richard has never taken
  QTCs and rides WAE CW 2026 without them; a log with no QTC lines is
  valid. If ever built: a receive window shaped for CW copy speed (series
  header plus time/call/serial rows), storage carrying the QTC
  transmission's own time and band (the rules require logging those), the
  10-per-station quota, and Cabrillo `QTC:` lines (QRG, MODE, DATE, TIME,
  CALL-RX, QTC-GRP, CALL-TX, TIME-QSO, CALL-QSO, NR-QSO). Build and
  practice it calmly before a WAE SSB/RTTY edition, never mid-contest.

## Safety / etiquette

The logbook never transmits and never changes radio state except
operator-triggered CW macros via TCI. Nothing leaves the machine without an explicit
action or an explicitly enabled sync service; callbook/QSL credentials live in
the secret service, not in config files. Richard's global rule applies: consent
before any major or irreversible step — a destructive log operation (mass
delete, re-import over existing data) always confirms first, and every upload
to an external QSL service is user-triggered in v1 (no silent background sync).
