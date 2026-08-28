/* qso_row.h — GObject wrapper around LogflQso for GListStore/GtkColumnView.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_QSO_ROW_H
#define LOGFL_QSO_ROW_H

#include <glib-object.h>

#include "log_store.h"

G_BEGIN_DECLS

#define LOGFL_TYPE_QSO_ROW (logfl_qso_row_get_type ())
G_DECLARE_FINAL_TYPE (LogflQsoRow, logfl_qso_row, LOGFL, QSO_ROW, GObject)

/* Takes ownership of qso. */
LogflQsoRow *logfl_qso_row_new (LogflQso *qso);
const LogflQso *logfl_qso_row_qso (LogflQsoRow *row);

/* Mutable access for in-place field edits (same pointer as qso()). */
LogflQso *logfl_qso_row_qso_mut (LogflQsoRow *row);

/* Replace the wrapped QSO (takes ownership of qso). Used after store update. */
void logfl_qso_row_replace (LogflQsoRow *row, LogflQso *qso);

/* Contest score annotation (LOG-3) — transient UI state computed by the
 * window's rescore pass, never part of the stored QSO. mult is the label
 * of the multiplier(s) this QSO brought first, NULL for none. */
void        logfl_qso_row_set_score (LogflQsoRow *row, int points,
                                     const char *mult);
void        logfl_qso_row_clear_score (LogflQsoRow *row);
gboolean    logfl_qso_row_scored (LogflQsoRow *row);
int         logfl_qso_row_points (LogflQsoRow *row);
const char *logfl_qso_row_mult (LogflQsoRow *row);

G_END_DECLS

#endif /* LOGFL_QSO_ROW_H */
