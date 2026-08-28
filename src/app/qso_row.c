/* qso_row.c — GObject wrapper around LogflQso (see qso_row.h).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "qso_row.h"

struct _LogflQsoRow {
  GObject parent_instance;
  LogflQso *qso;
  gboolean scored;             /* contest score annotation is valid */
  int points;
  char *mult;                  /* new-multiplier label(s); NULL = none */
};

G_DEFINE_FINAL_TYPE (LogflQsoRow, logfl_qso_row, G_TYPE_OBJECT)

static void
logfl_qso_row_finalize (GObject *obj)
{
  LogflQsoRow *self = LOGFL_QSO_ROW (obj);
  g_clear_pointer (&self->qso, logfl_qso_free);
  g_clear_pointer (&self->mult, g_free);
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

void
logfl_qso_row_set_score (LogflQsoRow *row, int points, const char *mult)
{
  g_return_if_fail (LOGFL_IS_QSO_ROW (row));
  row->scored = TRUE;
  row->points = points;
  g_free (row->mult);
  row->mult = g_strdup (mult);
}

void
logfl_qso_row_clear_score (LogflQsoRow *row)
{
  g_return_if_fail (LOGFL_IS_QSO_ROW (row));
  row->scored = FALSE;
  row->points = 0;
  g_clear_pointer (&row->mult, g_free);
}

gboolean
logfl_qso_row_scored (LogflQsoRow *row)
{
  return row->scored;
}

int
logfl_qso_row_points (LogflQsoRow *row)
{
  return row->points;
}

const char *
logfl_qso_row_mult (LogflQsoRow *row)
{
  return row->mult;
}
