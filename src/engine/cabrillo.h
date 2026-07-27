/* cabrillo.h — Cabrillo v3 contest log export (docs/SCOPE.md, contests).
 *
 * Emits the WWROF Cabrillo 3.0 shape (verified against wwrof.org and the
 * EUHFC sample template, 2026-07-28): a tag header, chronological
 * "QSO: freq mo yyyy-mm-dd hhmm call rst exch call rst exch" lines and
 * END-OF-LOG. HF frequencies are kHz (the actual QRG when the QSO has
 * one, the band's generic edge otherwise — per the EUHFC note); VHF+
 * bands use the Cabrillo band designators. Modes map CW→CW, SSB/AM→PH,
 * FM→FM, RTTY→RY, digital→DG.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_CABRILLO_H
#define LOGFL_CABRILLO_H

#include "log_store.h"

G_BEGIN_DECLS

/* Header fields. contest + callsign are required; every other field is
 * written only when non-empty. CREATED-BY is stamped automatically. */
typedef struct {
  const char *contest;         /* CONTEST:, e.g. "EUHFC" */
  const char *callsign;        /* CALLSIGN: used in the contest */
  const char *cat_operator;    /* SINGLE-OP / MULTI-OP / CHECKLOG */
  const char *cat_band;        /* ALL / 160M / 80M / … */
  const char *cat_power;       /* HIGH / LOW / QRP */
  const char *cat_mode;        /* MIXED / CW / SSB / RTTY / DIGI / FM */
  const char *cat_transmitter; /* ONE / TWO / LIMITED / UNLIMITED / SWL */
  const char *cat_assisted;    /* ASSISTED / NON-ASSISTED */
  const char *claimed_score;   /* integer text, no separators */
  const char *club;
  const char *operators;
  const char *name;
  const char *email;
  const char *location;        /* ARRL section or "DX" */
  const char *grid;            /* GRID-LOCATOR: */
  const char *soapbox;
} LogflCabrilloOpts;

/* Renders the contest's QSOs (oldest first, as the spec requires) into a
 * Cabrillo text. n_exported (optional) reports the QSO count. */
char *logfl_cabrillo_export (LogflStore *s, gint64 contest_id,
                             const LogflCabrilloOpts *o,
                             guint *n_exported, GError **error);

gboolean logfl_cabrillo_export_file (LogflStore *s, gint64 contest_id,
                                     const char *path,
                                     const LogflCabrilloOpts *o,
                                     guint *n_exported, GError **error);

G_END_DECLS

#endif /* LOGFL_CABRILLO_H */
