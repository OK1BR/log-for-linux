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

G_END_DECLS

#endif /* LOGFL_QSO_ROW_H */
