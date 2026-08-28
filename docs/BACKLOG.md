# Log for Linux — backlog

The single work queue for this app: shortcomings, ideas for new features and
bugs reported from real operation. `docs/SCOPE.md` says what the app **is** and
why it is built that way; this file says what is **queued, in progress or just
done**. When the two disagree, that is itself a backlog item.

## How things get in here

- **Found during a contest or live operation** — first written up in
  `docs/CONTEST-NOTES-<date>.md` (raw observation + analysis, no code touched
  while operating), then triaged into an item here.
- **Reported by someone else** — GitHub issue stays the conversation with the
  reporter; the item here mirrors it and carries the `gh#N` link, so one list
  still shows all the work.
- **Own idea / design gap** — straight in, marked `idea`.

## Item format

```
### LOG-N — one-line title
- **Type:** bug | idea | debt · **Severity:** high | medium | low · **Status:** open | doing | done | deferred
- **Source:** who/where/when
- **Detail:** pointer to the full write-up
Short statement of the problem and where in the code it lives.
```

Severity is about the damage, not the effort: `high` = wrong data or something
that leaves the machine wrong; `medium` = gets in the operator's way;
`low` = cosmetic or log noise.

---

## Open — bugs

### LOG-1 — Exchange fields do not upper-case while typing
- **Type:** bug · **Severity:** medium · **Status:** open
- **Source:** Richard, live, YO DX HF 2026-08-22
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §1

Stored data is fine (`logfl_exch_apply()` upper-cases on the way in, verified
against the real YO DX log and both exports); what the operator sees mid-QSO is
not. `entry_force_upper()` (`win.c:241`) is attached to the Call entry and to
the Call column of the cell editor only. The exchange rows (`win.c:2837`) and
the Sent serial (`win.c:2830`) are plain `mk_entry()`. The reasoning already
written down for the callsign at `win.c:214` applies verbatim: in a contest the
field is typed blind, with Caps Lock in an unknown state.

## Open — ideas

### LOG-3 — Contest score and multipliers, mult visible per QSO
- **Type:** idea · **Severity:** — · **Status:** open
- **Source:** Richard 2026-08-08 (`SCOPE.md:387`), re-raised live 2026-08-22
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §2

`SCOPE.md:387` already carries the IDEA: live claimed score, Cabrillo
`CLAIMED-SCORE`, per-contest QSO points, multiplier tracking per band. Live
operation added two things to it: the priority (score is counted by hand today)
and a new requirement — seeing **on the QSO row** that it brought a new
multiplier, which the IDEA does not cover. Needs a machine-readable points rule
in `exch_def` (today YO DX's 8/4/2/1 structure exists only as a comment at
`contest.c:465`) and a new `COL_*` column.

## Open — debt

### LOG-5 — SCOPE's Cabrillo section no longer matches the code
- **Type:** debt · **Severity:** low · **Status:** open
- **Source:** fallout of LOG-4, 2026-08-23

`SCOPE.md:337–340` still says the `CATEGORY-*` values are "persisted in
settings.ini `[cabrillo]`". Since LOG-4 that is only true for Operator, Power,
Transmitter and Assisted — Band and Mode are derived from the logged QSOs.

## Done

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
Live check of the dialog still pending.

## Roadmap

Milestones and their order live in `docs/SCOPE.md`; `docs/M3-CHECKLIST.md` is
the manual UI gate. This section only records what is next in practice:

1. **LOG-1** — two lines, immediately visible every contest.
2. **LOG-3** — the first feature-sized piece of work, scoped in SCOPE.md:387.

Done 2026-08-28: **LOG-2** (its live checks at the radio are listed in the
Done entry and ride along with the RTTY §7.3 pass).
