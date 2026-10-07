# Contributing

Thanks for your interest — but please read this before opening anything.

## Pull requests are not accepted

This project does **not** accept pull requests. A logbook holds years of
someone's contacts. A change that reads correctly can still lose a field on
the ADIF round-trip, mis-flag a dupe or send a Cabrillo log out with a wrong
exchange — and the operator finds out only after the QSL or the contest
result. Every change has to pass the gates in `meson test` (ADIF, store, dupe,
contest, Cabrillo, …), and anything that touches operating is checked live on
the air before its issue is closed. Unsolicited PRs will be closed without
review, regardless of quality.

## What IS welcome: issues

- **Wrong data** — the single most useful report: a field lost or changed on
  ADIF import or export, a wrong dupe or worked-before flag, a Cabrillo line a
  contest robot rejected. Attach the offending ADIF record or Cabrillo line;
  callsigns can be anonymised.
- **Bug reports** — with your distro, the `log-for-linux` version, what it was
  talking to (`sdr-for-linux` over TCI, WSJT-X / JTDX over UDP) and steps to
  reproduce. Logs from a terminal run help a lot.
- **Feature requests** — especially from real operating experience
  (contesting, DXing, digimodes).

## Questions and conversation: Discussions

Not everything is a bug report. For the rest, use
[Discussions](https://github.com/OK1BR/log-for-linux/discussions):

- **Q&A** — setup and operating questions: importing an old log, connecting
  to the radio or to WSJT-X, contest presets and categories. Not sure whether
  something is a bug? Start here. An answered question stays findable for the
  next operator with the same problem.
- **Ideas** — something you'd like to talk through before it becomes a
  concrete feature request.
- **Show and tell** — your station, your log, the contest you ran with it.

Issues stay for concrete bugs and requests.

73 de OK1BR
