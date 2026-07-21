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

- [x] Window opens without GLib/GTK criticals or warnings on a clean start
- [x] Title “Log for Linux”; subtitle shows `N QSO · M calls` (0/0 on empty DB)
- [x] UTC clock ticks once per second (`HH:MM:SS UTC · DD.MM.YYYY`)
- [x] Menu: Import ADIF…, Export ADIF…, About (version + SQLite + DB path)

## Entry row

- [x] Focus starts in Call
- [x] Defaults: band **40m**, mode **CW**, RST **599/599**
- [x] Mode **SSB/FM/AM** → RST defaults **59/59** (hand-edited value not stomped)
- [x] Mode **FT8/FT4** → RST fields clear (blank default)
- [x] Mode back to CW → RST back to 599 if still at a default value
- [x] Freq e.g. `14.074` auto-selects band **20m** (ADIF band table)
- [x] Freq outside all allocations leaves band selection unchanged
- [x] Enter in any entry field logs (same as “Log QSO”)
- [x] Empty call → toast “Callsign first”, focus stays on Call
- [x] Successful log → toast, Call/Name/Comment cleared, focus on Call, row in table

## Worked-B4

- [x] Call length &lt; 2 → hint empty
- [x] Unknown call (≥2 chars) → green “New call”
- [x] After logging once → yellow B4 line with totals / this band / band+mode / last date
- [x] Changing band or mode refreshes the counts

## Duplicate confirm

- [x] Re-log same call+band+mode within ~5 min → “Duplicate?” dialog
- [x] Default / Escape = **Cancel** (QSO not logged)
- [x] “Log anyway” inserts a second QSO
- [x] While dialog is open, another Enter → toast “Confirm the previous QSO first”

## Table & search

- [x] Columns: UTC, Call, Band, MHz, Mode, RST, Name, Comment
- [x] Newest QSO on top
- [x] Search filters by call / name / QTH / comment (engine-backed)
- [x] Clearing search restores full list
- [x] Typing quickly does not stutter (250 ms debounce)

## Edit (inline cells)

- [x] Single-click a cell opens its editor; the row is never selected/highlighted
- [x] Enter commits; value stays after reload/search
- [x] Esc, click outside the cell, focus-out or scroll cancels (value reverts)
- [x] UTC cell: `DD.MM.YY HH:MM` accepted; garbage shows toast and reverts
- [x] MHz cell: empty clears freq; valid number stores
- [x] RST cell: `599/599` form; placeholders `—` mean unset
- [x] No pencil / entry-strip edit — entry row is for new QSOs only

## Delete

- [x] Right-click anywhere on a row → confirm dialog directly (no context menu)
- [x] Confirm dialog; default = **Cancel**
- [x] Confirm Delete removes row; counters update
- [x] Second delete while dialog open is ignored
- [x] No trash toolbar icon

## ADIF

- [x] Import a small `.adi` → toast with imported / dups skipped / bad; table refreshes
- [x] Re-import same file → dups skipped (exact timestamp), no silent mass duplicates
- [x] Export → file written; re-import of that export is stable (no data loss on modeled fields)
- [x] Cancel on file dialog leaves the log unchanged

## Failure paths (optional)

- [ ] Unreadable ADIF path / bad content → toast, app stays up
- [ ] (Hard) Unwritable DB dir → dialog “Cannot open log”, no process abort

## Known issues at sign-off (not blocking, tracked for follow-up)

- ~~One `GLib-GObject-CRITICAL: g_object_unref` on window close~~ —
  **FIXED same day**: `gtk_column_view_new` is transfer-full on its model,
  so dispose dropped a ref the view owned; the ctor now gets its own ref.
  Reproduced and verified gone via `LOGFL_AUTOCLOSE_MS` (env hook that
  closes the window programmatically) + `G_DEBUG=fatal-criticals` + gdb.
- GTK emits `GtkImage … baselines must be inside the widget size` warnings
  when file dialogs open (origin GTK vs. ours not yet determined).

## Sign-off

| | |
|--|--|
| Date | 2026-07-21 |
| Tester | Richard / OK1BR |
| Headless `meson test` | 6/6 OK (engine, store, adif, macros, tci, udp) |
| Manual checklist | ☑ complete (optional failure paths not exercised) |

Signed off — **M3 IMPLEMENTED** is marked in `docs/SCOPE.md`; the app is v1
for daily use. Note: M4–M6 (TCI, macros v2, WSJT-X UDP) and edit-saved-QSO
landed in code after the original M3 checklist was written — this list
remains the manual UI gate for the core logbook. Next feature milestone in
the planned order: **M7 callbook**.
