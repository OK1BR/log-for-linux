# Log for Linux — backlog (archive)

**The work queue moved to GitHub Issues on 2026-09-18:**
<https://github.com/OK1BR/log-for-linux/issues> — bugs, ideas, debt and
everything still waiting for a live check are issues there. This file stays
as the archive of the `LOG-N` items from before the move; nothing new is
added here. `docs/SCOPE.md` still says what the app **is** and why it is
built that way; when SCOPE and the code disagree, that is itself an issue.

## How things get in now

- **Found during a contest or live operation** — first written up in
  `docs/CONTEST-NOTES-<date>.md` (raw observation + analysis, no code touched
  while operating), then triaged into issues that point back at the note.
- **Reported by someone else** — their GitHub issue is the item; no mirror.
- **Own idea / design gap** — an issue labelled `enhancement` or `debt`. A
  feature too big for an issue body gets its own `docs/<NAME>-SCOPE.md`
  (the zadání, versioned with the code); the issue links to it.

## Labels

Type: `bug`, `enhancement` (the former "idea"), `debt`. Severity is about the
damage, not the effort: `severity: high` = wrong data or something that
leaves the machine wrong; `severity: medium` = gets in the operator's way;
`severity: low` = cosmetic or log noise.

`needs-live-check` = done in code and the gate is green, but the issue
**stays open until the behaviour was seen live**; `at-the-radio` marks the
checks that need the rig. A commit closes its issue with `Fixes #N` only
when nothing is left to verify live — otherwise it says `Refs #N` and the
issue is closed by hand once the check passed.

---

## Archive — LOG-1…LOG-5 (done before the move)

### LOG-3 — Contest score and multipliers, mult visible per QSO
- **Type:** idea · **Severity:** — · **Status:** done 2026-08-28
- **Source:** Richard 2026-08-08 (SCOPE IDEA), re-raised live 2026-08-22
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §2; full write-up now in
  SCOPE's "Contest score calculation" entry

Implemented as machine-readable `points=` / `mult=` rules in the contest's
exch_def + an engine scorer (`logfl_contest_score`), with every preset rule
re-verified 2026-08-28 from the sponsors' official rules (three parallel
research passes with verbatim quotes; SARTG ships points-only because its
multiplier wording is ambiguous, WAE scores without the deferred QTCs).
UI: live "pts × mult = total" in the contest subtitle, computed Pts/Mult
table columns (Mult shows what the QSO brought first — the requirement live
operation added), CLAIMED-SCORE prefilled in the Cabrillo dialog, startup
backfill for existing contests. Gate: 10 new /contest/score/* cases against
the bundled cty.dat, including a mult-namespace collision (YO county CT vs
Portugal's prefix) caught by running the scorer on the real YO DX log —
which scores 107 QSO → 307 pts × 49 mult = 15043.

**Live checks pending:** subtitle + Pts/Mult columns and Cabrillo claimed
score over the real DB after restart (the backfill writes the rules on
first start); Richard's own hand-counted YO DX score vs the 15043 estimate.
Tracked in #2.

### LOG-5 — SCOPE's Cabrillo section no longer matches the code
- **Type:** debt · **Severity:** low · **Status:** done 2026-08-28
- **Source:** fallout of LOG-4, 2026-08-23

SCOPE's Cabrillo entry said all `CATEGORY-*` values are "persisted in
settings.ini `[cabrillo]`". Since LOG-4 that is only true for Operator,
Power, Transmitter and Assisted; the entry now records that Band and Mode
are derived from the logged QSOs, why, and that both stay editable.

### LOG-1 — Exchange fields do not upper-case while typing
- **Type:** bug · **Severity:** medium · **Status:** done 2026-08-28
- **Source:** Richard, live, YO DX HF 2026-08-22
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §1

Stored data was always fine (`logfl_exch_apply()` and `parse_exch_cell()`
upper-case on the way in, verified against the real YO DX log and both
exports); what the operator saw mid-QSO was not. Fixed by attaching
`entry_force_upper()` to the same fields the store normalizes: the exchange
entries and Sent in the entry row (`rebuild_exch_fields()`), and the
Sent/Rcvd columns of the table cell editor next to the existing Call one.
No per-field-type branching — `g_utf8_strup` is a no-op on digits, so
serial-only fields are unaffected. Gate: build + `meson test` 10/10 (UI
path). Live check: typing `bu` into Nr/County must show `BU`; serial
prefill and the row clear after logging must keep working.

### LOG-2 — Exchange from the previous station survives a spot switch
- **Type:** bug · **Severity:** high · **Status:** done 2026-08-28, committed `935a07d`
- **Source:** Richard, live, YO DX HF 2026-08-22
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §3

Clicking spot A, typing the received exchange, then clicking spot B replaced
the callsign but left A's exchange in the fields — a wrong exchange could be
logged against B. Fixed by widening the "operator touched it" contract from
Call to the whole QSO row: a new `changed` handler (`on_row_edited_drop_spot`)
on RST s/r, Sent, every exchange field, Name and Comment drops
`call_from_spot` as soon as the operator types. **Chosen semantics: once the
row is typed into, a spot click is a no-op** — spot B does not fill until the
row is logged or cleared by hand; nothing ever swaps the call out from under
a half-copied exchange. Programmatic writes (row reset, serial/RST prefill,
TCI mode echo rewriting RST defaults) run under a new `syncing_row` guard
(plus the existing `syncing_tci`) so they keep the mark alive; Call's own
handler stays deliberately unguarded — `tci_apply_spot` re-arms after its
write. Second path fixed too: QSY with an open cell editor now resets the
whole row (`entry_reset_defaults`), only the focus grab is skipped
(`clear_entry_row` checks `cell_edit_box`). SCOPE.md:172/179 wording
tightened to match.

Verified: build + `meson test` 10/10 (engine tests cannot see this UI path).
**Live checks pending at the radio:** (1) spot A → type exchange → spot B:
the call must NOT change and the row stays the operator's (the contest note's
original "exchange must disappear" phrasing predates the chosen design);
(2) QSY >200 Hz with a cell editor open: whole row clears, focus stays in the
cell; (3) spot click on a different-mode station: the TCI mode flip rewrites
RST defaults and must NOT kill the prefill — a second spot click still
replaces the call; (4) after logging a QSO focus still lands in Call.
Tracked in #1.

### LOG-4 — Cabrillo CATEGORY-MODE/BAND came from the previous contest
- **Type:** bug · **Severity:** high · **Status:** done 2026-08-23, committed `9b99622`
- **Source:** Richard, after YO DX HF submission
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §4

A 107-QSO all-CW log was exported and submitted with `CATEGORY-MODE: RTTY`,
inherited from the previous contest (SARTG WW RTTY) through settings.ini.
Richard corrected the header by hand and resubmitted. Fixed by deriving
CATEGORY-MODE and CATEGORY-BAND from the contest's own QSOs
(`logfl_cabrillo_categories_from_log()`); the remembered value survives only as
a fallback for a contest with no QSOs yet. Both stay editable — the log proves
what was worked, not which category was entered. Gate:
`/cabrillo/categories-from-log`; verified against the real log (CW / 20M).
Live check of the dialog still pending (tracked in #3).

## Roadmap

Milestones and their order live in `docs/SCOPE.md`; `docs/M3-CHECKLIST.md` is
the manual UI gate. What is next in practice is the open issue list. At the
move (2026-09-18) everything from YO DX HF (LOG-1…LOG-5) was done in code and
the open issues were all live verification: #1 LOG-2 spot behavior, #2 LOG-3
score UI, #3 LOG-4 export dialog, #4 the RTTY §7 step 3 pass, #5 the two
optional M3 failure paths.
