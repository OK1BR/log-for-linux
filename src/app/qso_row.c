/* qso_row.c — GObject wrapper around LogflQso (see qso_row.h).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "qso_row.h"

struct _LogflQsoRow {
  GObject parent_instance;
  LogflQso *qso;
};

G_DEFINE_FINAL_TYPE (LogflQsoRow, logfl_qso_row, G_TYPE_OBJECT)

static void
logfl_qso_row_finalize (GObject *obj)
{
  LogflQsoRow *self = LOGFL_QSO_ROW (obj);
  g_clear_pointer (&self->qso, logfl_qso_free);
  G_OBJECT_CLASS (logfl_qso_row_parent_class)->finalize (obj);
}

static void
logfl_qso_row_class_init (LogflQsoRowClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = logfl_qso_row_finalize;
}

static void
logfl_qso_row_init (LogflQsoRow *self)
{
  (void) self;
}

LogflQsoRow *
logfl_qso_row_new (LogflQso *qso)
{
  LogflQsoRow *row = g_object_new (LOGFL_TYPE_QSO_ROW, NULL);
  row->qso = qso;
  return row;
}

const LogflQso *
logfl_qso_row_qso (LogflQsoRow *row)
{
  return row->qso;
}

LogflQso *
logfl_qso_row_qso_mut (LogflQsoRow *row)
{
  return row->qso;
}

void
logfl_qso_row_replace (LogflQsoRow *row, LogflQso *qso)
{
  g_return_if_fail (LOGFL_IS_QSO_ROW (row));
  g_return_if_fail (qso != NULL);
  if (row->qso == qso)
    return;
  g_clear_pointer (&row->qso, logfl_qso_free);
  row->qso = qso;
}
