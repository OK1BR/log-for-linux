# RTTY keying integration — scope plan (zadání, 2026-08-15)

Requested by Richard on 2026-08-15, the day the radio side landed:
**sdr-for-linux has a native RTTY mode** (its `docs/RTTY-SCOPE.md`, commit
f38aafd — direct-FSK modulator keyed from text over the TCI family extension
`rtty_macros:`/`rtty_macros_stop;`, live-keyed the same day). Goal here: an
**F-key macro in this logbook keys a complete RTTY exchange** through that
command — and nothing else changes. The macro bar, banks, ESM, placeholder
expansion and the QSO entry flow are already mode-agnostic and stay untouched;
this scope is the transport dispatch plus the CW-isms that must not leak into
RTTY. This is §4 of the sdr-for-linux RTTY scope, expanded against this
repo's actual code (recon 2026-08-15, all cites verified first-hand).

## 0. Milestone gate

`log-tci-test` grows RTTY coverage (mock TCI server, engine-level — no GUI):

- `logfl_tci_mode_to_log("rtty")` returns `"RTTY"` (today NULL);
- `logfl_tci_client_rtty_send()` emits `rtty_macros:0,<text>;` with the SAME
  treatment as the CW path: reserved `:`/`,`/`;` scrubbed to spaces and the
  **deliberate leading space** kept (tci_client.c:536-541 — the word-gap
  rule; sdr-for-linux's rtty_gen implements the identical
  skip-when-idle/keep-when-queued semantics, offline-gated there);
- `logfl_tci_client_rtty_stop()` emits `rtty_macros_stop;`.

The GUI dispatch (macro_run routing, cut-number suppression) is not
gate-reachable (GTK); it is verified in the live pass (§8.3).

## 1. Findings (recon 2026-08-15)

- **Every keying path converges on `macro_run` (win.c:1129)**: macro-bar
  click (:1239), ESM Enter (:1452), F1-F8 (:1657), and the stop slot / Esc
  (:1692 → the stop branch :1139). One function to teach RTTY.
- **The mode gate already admits RTTY — and then keys Morse.** win.c:1220
  lets modes `"CW"` and `"RTTY"` through, and :1228 unconditionally calls
  `logfl_tci_client_cw_send()`: an RTTY macro TODAY transmits the text as
  CW. The gate reads the **log mode dropdown** (`dd_selected(mode_dd)`),
  not the radio's TCI mode — the dropdown is the operator's authority
  (prefilled from the radio, overridable).
- **Mode prefill misses RTTY.** win.c:431 sets the dropdown from
  `logfl_tci_mode_to_log(st->mode)`; the map (tci_client.c:580) has
  `digu/digl → FT8` and **no `rtty` case** → NULL → the dropdown goes stale
  when the radio switches to RTTY.
- **Cut numbers are a CW convention applied without a mode check.**
  win.c:1175 cuts on `settings.cw_cut_numbers` alone — in RTTY, "5NN"
  is nonsense on the wire; digits must go verbatim.
- **The Ctrl+K free-text window is CW-hardcoded**: send
  (on_cw_text_activate, win.c:1506), its Stop button (:1520) and its Esc
  (:1537) all use the cw_* client calls.
- **PgUp/PgDn keyer speed** (win.c:1663 → cw_speed_bump :350 →
  `cw_macros_speed`) is CW-only by nature — RTTY is fixed 45.45 Bd on the
  radio side, no speed to set.
- **Radio-side status (2026-08-15):** keying live-verified at 1 W on the
  G2E (4 overs: clean KEY RTTY/UNKEY, steady fwd, SWR 1.00). The
  own-TX skimmer-decode loop needs more drive (T/R relay + 31 dB TX
  attenuators bury the leak at 0.36 W) — Richard is testing that himself;
  it does not block this scope.

## 2. Changes (proposed)

**TCI client** (`src/engine/tci_client.[ch]`):
- `logfl_tci_client_rtty_send()` / `logfl_tci_client_rtty_stop()` — clones
  of the cw pair on the `rtty_macros` family, identical scrub + leading
  space (§0).
- `logfl_tci_mode_to_log()` gains `rtty → "RTTY"`.

**macro_run** (`src/app/win.c`):
- Dispatch on the mode dropdown: `"CW"` → `cw_send`, `"RTTY"` →
  `rtty_send`; the refuse-toast for other modes stays (text updated).
- Cut numbers apply **only when the mode is CW** — RTTY sends the digits
  verbatim, no new setting.
- The stop branch (:1139) sends **both** `cw_stop` + `rtty_stop` — Esc is
  the panic key ("stop, then close", win.c:1535); a stop of an idle
  generator is a no-op on the radio, so firing both is free and never
  leaves the wrong queue running.

**Esc + Ctrl+K window**: every stop site (:1139, :1520, :1537) fires both
stops; the text window's send goes mode-aware through the same dispatch;
its title and the toasts generalize ("Send text", "TX stop").

**Unchanged by decision**: PgUp/PgDn (CW WPM only), macro banks/ESM/
placeholders, contest/exchange logic, ADIF/Cabrillo (RTTY is already a log
mode — `modes[]` win.c:46, Cabrillo :3660), settings surface (no new keys).

## 3. Reference sequence (one contest QSO, the whole family)

skimmer spots RTTY CQ (pair centre) → operator clicks → sdr-for-linux tunes
dial = pair centre, relays the click → this logbook prefills Call, the mode
dropdown reads RTTY (the new mapping) → F2 → `{CALL} 599 {NR}` expands →
`rtty_macros:0, DL1ABC 599 001;` → the radio's rtty_gen keys direct FSK
through tx_gate → mark tail → RX. Esc here → `rtty_macros_stop;` (+
`cw_macros_stop;`) → the radio ramps down within one block.

## 4. Files

| File | Change |
|---|---|
| `src/engine/tci_client.c/.h` | rtty_send/rtty_stop + mode map `rtty` |
| `src/app/win.c` | macro_run dispatch, CW-only cut numbers, dual stops, Ctrl+K window + toasts |
| `src/tci_test.c` | §0 gate coverage |

## 5. Risks & mitigations

- **Older sdr-for-linux** (pre-f38aafd) ignores unknown TCI commands: an
  RTTY macro would toast "TX …" and key nothing. Family upgrades together;
  accepted, not handled.
- **Dropdown says RTTY, radio sits in another mode**: the radio side
  refuses (`tci_rtty_send` is gated on its RTTY mode), nothing transmits —
  same failure class as today's CW path; acceptable, the live pass
  documents the behaviour.
- **Double stop** on Esc: both commands are idempotent no-ops when idle
  (verified in sdr-for-linux tci_server); no radio-side churn.

## 6. Decisions

**Given by the assignment:** key RTTY from the existing macros through
`rtty_macros:`; everything around the macros stays as it is.

**Confirmed by Richard 2026-08-15 (whole package), with one addition: verify
the mode really syncs from the radio — when the SDR lights up RTTY, the
dropdown must never sit on CW. Covered: the radio broadcasts
`modulation:0,rtty;` on every change AND in the handshake before `ready;`
(tci_server.c:254/:359), the client tracks it unconditionally, and the new
`rtty → "RTTY"` mapping was the only missing link; gate-tested end to end
(mock push → state callback → map).**
- **A.** Dispatch authority = the log mode dropdown (the existing gate's
  source), not the radio's reported mode.
- **B.** Cut numbers auto-suppressed in RTTY — no new setting.
- **C.** Stop/Esc always fires BOTH cw+rtty stops (panic semantics).
- **D.** The Ctrl+K text window becomes mode-aware; no separate RTTY
  window, labels generalized.
- **E.** No RTTY speed UI (45.45 Bd is fixed radio-side).

## 7. Implementation order (after consent)

1. `tci_client` pair + mode map + `log-tci-test` coverage (engine only,
   gate-verified offline). — **DONE 2026-08-15, gate 10/10.**
2. `win.c`: macro_run dispatch, CW-only cuts, dual stops, Ctrl+K + texts.
   — **DONE 2026-08-15** (builds; GUI paths await the live pass).
3. Live pass against sdr-for-linux ≥ f38aafd: F-key keys a real FSK over
   in RTTY mode, Esc aborts mid-over, prefill flips the dropdown when the
   radio switches to RTTY; then the first macro-keyed RTTY QSO on air.
   — **PENDING (Richard, at the radio).**
