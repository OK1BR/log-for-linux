# M3 — UI v1 manual gate checklist

Milestone goal (from `docs/SCOPE.md`): a usable logbook — entry row, QSO
table, search/filter, ADIF import/export, worked-B4 while typing. Headless
gates for M0–M2 stay green; this list is the UI gate that makes **v1**.

Build and open:

```
meson compile -C builddir && meson test -C builddir
./builddir/log-for-linux
```

DB path (About → debug info): `~/.local/share/log-for-linux/log.db`.

Mark each item when verified on a real desktop session.

## Launch & chrome

- [ ] Window opens without GLib/GTK criticals or warnings on a clean start
- [ ] Title “Log for Linux”; subtitle shows `N QSO · M calls` (0/0 on empty DB)
- [ ] UTC clock ticks once per second (`HH:MM:SS UTC · DD.MM.YYYY`)
- [ ] Menu: Import ADIF…, Export ADIF…, About (version + SQLite + DB path)

## Entry row

- [ ] Focus starts in Call
- [ ] Defaults: band **40m**, mode **CW**, RST **599/599**
- [ ] Mode **SSB/FM/AM** → RST defaults **59/59** (hand-edited value not stomped)
- [ ] Mode **FT8/FT4** → RST fields clear (blank default)
- [ ] Mode back to CW → RST back to 599 if still at a default value
- [ ] Freq e.g. `14.074` auto-selects band **20m** (ADIF band table)
- [ ] Freq outside all allocations leaves band selection unchanged
- [ ] Enter in any entry field logs (same as “Log QSO”)
- [ ] Empty call → toast “Callsign first”, focus stays on Call
- [ ] Successful log → toast, Call/Name/Comment cleared, focus on Call, row in table

## Worked-B4

- [ ] Call length &lt; 2 → hint empty
- [ ] Unknown call (≥2 chars) → green “New call”
- [ ] After logging once → yellow B4 line with totals / this band / band+mode / last date
- [ ] Changing band or mode refreshes the counts

## Duplicate confirm

- [ ] Re-log same call+band+mode within ~5 min → “Duplicate?” dialog
- [ ] Default / Escape = **Cancel** (QSO not logged)
- [ ] “Log anyway” inserts a second QSO
- [ ] While dialog is open, another Enter → toast “Confirm the previous QSO first”

## Table & search

- [ ] Columns: UTC, Call, Band, MHz, Mode, RST, Name, Comment
- [ ] Newest QSO on top
- [ ] Search filters by call / name / QTH / comment (engine-backed)
- [ ] Clearing search restores full list
- [ ] Typing quickly does not stutter (250 ms debounce)

## Edit (inline cells)

- [ ] Single-click does **not** select/highlight a row
- [ ] Double-click a **Call** cell → edit, Enter commits, value stays after reload/search
- [ ] Esc while editing a cell cancels (value reverts)
- [ ] UTC cell: `DD.MM.YY HH:MM` accepted; garbage shows toast and reverts
- [ ] MHz cell: empty clears freq; valid number stores
- [ ] RST cell: `599/599` form; placeholders `—` mean unset
- [ ] No pencil / entry-strip edit — entry row is for new QSOs only

## Delete

- [ ] Right-click a table row → context menu “Delete QSO…”
- [ ] Confirm dialog; default = Cancel
- [ ] Confirm Delete removes row; counters update
- [ ] Second delete while dialog open is ignored
- [ ] No trash toolbar icon

## ADIF

- [ ] Import a small `.adi` → toast with imported / dups skipped / bad; table refreshes
- [ ] Re-import same file → dups skipped (exact timestamp), no silent mass duplicates
- [ ] Export → file written; re-import of that export is stable (no data loss on modeled fields)
- [ ] Cancel on file dialog leaves the log unchanged

## Failure paths (optional)

- [ ] Unreadable ADIF path / bad content → toast, app stays up
- [ ] (Hard) Unwritable DB dir → dialog “Cannot open log”, no process abort

## Sign-off

| | |
|--|--|
| Date | |
| Tester | Richard / OK1BR |
| Headless `meson test` | 4/4 OK (engine, store, adif, tci) |
| Manual checklist | ☐ complete |

When this passes, mark **M3 IMPLEMENTED** in `docs/SCOPE.md` and treat the
app as v1 for daily use. Note: M4–M6 (TCI, macros v2, WSJT-X UDP) and
edit-saved-QSO already landed in code after the original M3 checklist was
written — this list remains the manual UI gate for the core logbook. Next
feature milestone after M3 sign-off in the planned order: **M7 callbook**.
