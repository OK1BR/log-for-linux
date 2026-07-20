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

- **M0 — scaffold.** Engine/app split, sqlite3 dependency, test harness.
  Gate: `meson test` runs a trivial engine test; the empty window launches.
- **M1 — log store.** Schema v1 + migrations, CRUD, worked-B4 query, dup check
  (same call+band+mode within a time window), stats counters.
  Gate: `log-store-test` — headless, covers schema migration from an empty and
  a v1 file, CRUD, dup/worked-B4 edge cases, and volume (50k synthetic QSOs).
- **M2 — ADIF import/export.** ADIF 3.1.x parser + writer over the store,
  unknown-field preservation, import dedup report.
  Gate: `log-adif-test` — round-trip byte-fidelity on modeled and unknown
  fields, plus a quirks corpus (real-world exports: case, line endings,
  missing `<EOH>`, odd whitespace).
- **M3 — UI v1: the usable logbook.** Entry row (call/RST/band/mode/…,
  UTC clock), QSO table (GtkColumnView, newest first), search/filter,
  ADIF import/export in the UI, worked-B4 indication while typing.
  Gate: manual checklist — this milestone is deliberately UI; everything under
  it is already gated headless. **This is v1.**
- **M4 — TCI integration.** Connect to `sdr-for-linux`, entry row pre-fills
  freq/mode from the live VFO, double-click on a logged QSO → QSY.
  Gate: `log-tci-test` — mock TCI server (skimmer house pattern); live check
  against the real radio.
- **M5 — WSJT-X UDP.** UDP server (default port 2237): decode `QSO Logged`
  into the store, answer status/worked-B4 (callsign highlight).
  Gate: `log-udp-test` — replay captured WSJT-X datagrams headless; live check
  logging one real FT8 QSO end to end.
- **M6 — callbook lookup.** QRZ.com XML (subscriber) / HamQTH (free) —
  name/QTH/grid auto-fill on callsign entry, on-disk cache, credentials in the
  keyring, never in config files.
  Gate: `log-callbook-test` against canned XML; live smoke against both APIs.
- **M7 — QSL sync.** LoTW: sign+upload via `tqsl` CLI, pull confirmations
  (`lotwreport.adi`) and mark QSLs; eQSL upload + inbox; Club Log upload.
  Per-QSO sent/confirmed state per service, retry-safe (idempotent re-upload).
  Gate: `log-qsl-test` over mocked endpoints; live check with a small batch.
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
