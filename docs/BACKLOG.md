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

### LOG-2 — Exchange from the previous station survives a spot switch
- **Type:** bug · **Severity:** high · **Status:** open
- **Source:** Richard, live, YO DX HF 2026-08-22
- **Detail:** `docs/CONTEST-NOTES-2026-08-22.md` §3

Clicking spot A, typing the received exchange, then clicking spot B replaces
the callsign but leaves A's exchange in the fields — a wrong exchange can be
logged against B. `on_call_changed_drop_spot` is connected to the Call entry
only (`win.c:5183`); the exchange rows built in `rebuild_exch_fields()` carry
no signal, so `call_from_spot` never drops and `tci_apply_spot()` rewrites just
the call. A second, independent path: with a table cell editor open, the QSY
branch (`win.c:453`) deliberately narrows the reset to clearing Call, leaving
RST/name/exchange behind.

Note this is also a gap in the written contract, not only in the code:
`SCOPE.md:181` states the intent — *"leftovers of a QSO that never happened
cannot leak into the next one"* — while `SCOPE.md:172` defines the prefill as
tracked *"until the operator touches Call"*. The code implements the letter and
misses the intent. Fixing this item should tighten that wording too.

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

### LOG-4 — Cabrillo CATEGORY-MODE/BAND came from the previous contest
- **Type:** bug · **Severity:** high · **Status:** done 2026-08-23 (not committed)
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

1. **LOG-2** — wrong data reaches the log; nothing else outranks that.
2. **LOG-1** — two lines, immediately visible every contest.
3. **LOG-3** — the first feature-sized piece of work, scoped in SCOPE.md:387.
