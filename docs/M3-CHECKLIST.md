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

## Delete

- [ ] Trash with no selection does nothing
- [ ] Selected row → confirm dialog; default = Cancel
- [ ] Confirm Delete removes row; counters update
- [ ] Second trash click while dialog open is ignored

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
| Headless `meson test` | 3/3 OK |
| Manual checklist | ☐ complete |

When this passes, mark **M3 IMPLEMENTED** in `docs/SCOPE.md` and treat the
app as v1 for daily use. Next planned milestone: **M4 TCI** (`sdr-for-linux`).
