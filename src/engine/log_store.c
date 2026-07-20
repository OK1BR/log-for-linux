/* log_store.c — SQLite-backed QSO store (see log_store.h).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "log_store.h"

#include <sqlite3.h>
#include <string.h>

struct _LogflStore {
  sqlite3 *db;
};

/* Column order shared by INSERT, UPDATE and SELECT — keep the three uses and
 * bind_qso()/row_to_qso() in sync when the schema grows. */
#define QSO_COLS \
  "ts, call, band, freq, mode, submode, rst_sent, rst_rcvd, " \
  "gridsquare, name, qth, tx_pwr, comment, " \
  "qsl_rcvd, qsl_sent, lotw_qsl_rcvd, lotw_qsl_sent, " \
  "eqsl_qsl_rcvd, eqsl_qsl_sent, station_callsign, my_gridsquare, extras"
#define QSO_N_COLS 22

static const char *schema_v1 =
  "CREATE TABLE qso ("
  "  id INTEGER PRIMARY KEY,"
  "  ts INTEGER NOT NULL,"
  "  call TEXT NOT NULL,"
  "  band TEXT NOT NULL,"
  "  freq REAL,"
  "  mode TEXT NOT NULL,"
  "  submode TEXT,"
  "  rst_sent TEXT, rst_rcvd TEXT,"
  "  gridsquare TEXT, name TEXT, qth TEXT,"
  "  tx_pwr REAL,"
  "  comment TEXT,"
  "  qsl_rcvd TEXT, qsl_sent TEXT,"
  "  lotw_qsl_rcvd TEXT, lotw_qsl_sent TEXT,"
  "  eqsl_qsl_rcvd TEXT, eqsl_qsl_sent TEXT,"
  "  station_callsign TEXT, my_gridsquare TEXT,"
  "  extras TEXT"
  ");"
  "CREATE INDEX idx_qso_ts ON qso (ts DESC);"
  "CREATE INDEX idx_qso_call ON qso (call);"
  "CREATE INDEX idx_qso_call_band_mode ON qso (call, band, mode);";

GQuark
logfl_store_error_quark (void)
{
  return g_quark_from_static_string ("logfl-store-error");
}

/* --- LogflQso ---------------------------------------------------------- */

LogflQso *
logfl_qso_new (void)
{
  return g_new0 (LogflQso, 1);
}

LogflQso *
logfl_qso_copy (const LogflQso *q)
{
  LogflQso *c = logfl_qso_new ();
  c->id = q->id;
  c->ts = q->ts;
  c->freq = q->freq;
  c->tx_pwr = q->tx_pwr;
  c->call = g_strdup (q->call);
  c->band = g_strdup (q->band);
  c->mode = g_strdup (q->mode);
  c->submode = g_strdup (q->submode);
  c->rst_sent = g_strdup (q->rst_sent);
  c->rst_rcvd = g_strdup (q->rst_rcvd);
  c->gridsquare = g_strdup (q->gridsquare);
  c->name = g_strdup (q->name);
  c->qth = g_strdup (q->qth);
  c->comment = g_strdup (q->comment);
  c->qsl_rcvd = g_strdup (q->qsl_rcvd);
  c->qsl_sent = g_strdup (q->qsl_sent);
  c->lotw_qsl_rcvd = g_strdup (q->lotw_qsl_rcvd);
  c->lotw_qsl_sent = g_strdup (q->lotw_qsl_sent);
  c->eqsl_qsl_rcvd = g_strdup (q->eqsl_qsl_rcvd);
  c->eqsl_qsl_sent = g_strdup (q->eqsl_qsl_sent);
  c->station_callsign = g_strdup (q->station_callsign);
  c->my_gridsquare = g_strdup (q->my_gridsquare);
  c->extras = g_strdup (q->extras);
  return c;
}

void
logfl_qso_free (LogflQso *q)
{
  if (!q)
    return;
  g_free (q->call);
  g_free (q->band);
  g_free (q->mode);
  g_free (q->submode);
  g_free (q->rst_sent);
  g_free (q->rst_rcvd);
  g_free (q->gridsquare);
  g_free (q->name);
  g_free (q->qth);
  g_free (q->comment);
  g_free (q->qsl_rcvd);
  g_free (q->qsl_sent);
  g_free (q->lotw_qsl_rcvd);
  g_free (q->lotw_qsl_sent);
  g_free (q->eqsl_qsl_rcvd);
  g_free (q->eqsl_qsl_sent);
  g_free (q->station_callsign);
  g_free (q->my_gridsquare);
  g_free (q->extras);
  g_free (q);
}

/* --- helpers ----------------------------------------------------------- */

static gboolean
sql_fail (sqlite3 *db, const char *what, GError **error)
{
  g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_SQL,
               "%s: %s", what, sqlite3_errmsg (db));
  return FALSE;
}

static gboolean
exec_simple (sqlite3 *db, const char *sql, GError **error)
{
  if (sqlite3_exec (db, sql, NULL, NULL, NULL) != SQLITE_OK)
    return sql_fail (db, sql, error);
  return TRUE;
}

static void
bind_str (sqlite3_stmt *st, int i, const char *s)
{
  if (s && *s)
    sqlite3_bind_text (st, i, s, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null (st, i);
}

static char *
col_str (sqlite3_stmt *st, int i)
{
  const unsigned char *t = sqlite3_column_text (st, i);
  return t ? g_strdup ((const char *) t) : NULL;
}

/* Normalized copy of a filter/query input; NULL stays NULL. */
static char *
norm_dup (const char *s, gboolean up)
{
  if (!s)
    return NULL;
  char *d = up ? g_ascii_strup (s, -1) : g_ascii_strdown (s, -1);
  return g_strstrip (d);
}

static gboolean
qso_normalize (LogflQso *q, GError **error)
{
  char *call = norm_dup (q->call, TRUE);
  char *band = norm_dup (q->band, FALSE);
  char *mode = norm_dup (q->mode, TRUE);

  if (!call || !*call || !band || !*band || !mode || !*mode || q->ts <= 0)
    {
      g_free (call);
      g_free (band);
      g_free (mode);
      g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID,
                   "QSO needs call, band, mode and a timestamp");
      return FALSE;
    }
  g_free (q->call); q->call = call;
  g_free (q->band); q->band = band;
  g_free (q->mode); q->mode = mode;
  return TRUE;
}

/* Binds the QSO_COLS values at indices first..first+QSO_N_COLS-1. */
static void
bind_qso (sqlite3_stmt *st, int first, const LogflQso *q)
{
  int i = first;
  sqlite3_bind_int64 (st, i++, q->ts);
  bind_str (st, i++, q->call);
  bind_str (st, i++, q->band);
  if (q->freq > 0)
    sqlite3_bind_double (st, i++, q->freq);
  else
    sqlite3_bind_null (st, i++);
  bind_str (st, i++, q->mode);
  bind_str (st, i++, q->submode);
  bind_str (st, i++, q->rst_sent);
  bind_str (st, i++, q->rst_rcvd);
  bind_str (st, i++, q->gridsquare);
  bind_str (st, i++, q->name);
  bind_str (st, i++, q->qth);
  if (q->tx_pwr > 0)
    sqlite3_bind_double (st, i++, q->tx_pwr);
  else
    sqlite3_bind_null (st, i++);
  bind_str (st, i++, q->comment);
  bind_str (st, i++, q->qsl_rcvd);
  bind_str (st, i++, q->qsl_sent);
  bind_str (st, i++, q->lotw_qsl_rcvd);
  bind_str (st, i++, q->lotw_qsl_sent);
  bind_str (st, i++, q->eqsl_qsl_rcvd);
  bind_str (st, i++, q->eqsl_qsl_sent);
  bind_str (st, i++, q->station_callsign);
  bind_str (st, i++, q->my_gridsquare);
  bind_str (st, i++, q->extras);
}

/* Reads a row of "SELECT id, " QSO_COLS. */
static LogflQso *
row_to_qso (sqlite3_stmt *st)
{
  LogflQso *q = logfl_qso_new ();
  int i = 0;
  q->id = sqlite3_column_int64 (st, i++);
  q->ts = sqlite3_column_int64 (st, i++);
  q->call = col_str (st, i++);
  q->band = col_str (st, i++);
  q->freq = sqlite3_column_type (st, i) == SQLITE_NULL
              ? 0 : sqlite3_column_double (st, i);
  i++;
  q->mode = col_str (st, i++);
  q->submode = col_str (st, i++);
  q->rst_sent = col_str (st, i++);
  q->rst_rcvd = col_str (st, i++);
  q->gridsquare = col_str (st, i++);
  q->name = col_str (st, i++);
  q->qth = col_str (st, i++);
  q->tx_pwr = sqlite3_column_type (st, i) == SQLITE_NULL
                ? 0 : sqlite3_column_double (st, i);
  i++;
  q->comment = col_str (st, i++);
  q->qsl_rcvd = col_str (st, i++);
  q->qsl_sent = col_str (st, i++);
  q->lotw_qsl_rcvd = col_str (st, i++);
  q->lotw_qsl_sent = col_str (st, i++);
  q->eqsl_qsl_rcvd = col_str (st, i++);
  q->eqsl_qsl_sent = col_str (st, i++);
  q->station_callsign = col_str (st, i++);
  q->my_gridsquare = col_str (st, i++);
  q->extras = col_str (st, i++);
  return q;
}

/* --- open / migrate ---------------------------------------------------- */

static gboolean
store_migrate (sqlite3 *db, GError **error)
{
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (db, "PRAGMA user_version;", -1, &st, NULL)
        != SQLITE_OK)
    return sql_fail (db, "user_version", error);
  int v = sqlite3_step (st) == SQLITE_ROW ? sqlite3_column_int (st, 0) : 0;
  sqlite3_finalize (st);

  if (v == LOGFL_STORE_SCHEMA_VERSION)
    return TRUE;
  if (v > LOGFL_STORE_SCHEMA_VERSION)
    {
      g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_SCHEMA,
                   "log schema v%d is newer than this app (knows v%d) — "
                   "refusing to touch it", v, LOGFL_STORE_SCHEMA_VERSION);
      return FALSE;
    }

  if (!exec_simple (db, "BEGIN IMMEDIATE;", error))
    return FALSE;
  if (v == 0 && !exec_simple (db, schema_v1, error))
    goto rollback;
  if (!exec_simple (db, "PRAGMA user_version = 1;", error))
    goto rollback;
  return exec_simple (db, "COMMIT;", error);

rollback:
  sqlite3_exec (db, "ROLLBACK;", NULL, NULL, NULL);
  return FALSE;
}

LogflStore *
logfl_store_open (const char *path, GError **error)
{
  sqlite3 *db = NULL;
  if (sqlite3_open_v2 (path, &db,
                       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL)
        != SQLITE_OK)
    {
      g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_OPEN,
                   "cannot open %s: %s",
                   path, db ? sqlite3_errmsg (db) : "out of memory");
      sqlite3_close (db);
      return NULL;
    }

  /* WAL is a no-op on :memory: — harmless. */
  if (!exec_simple (db, "PRAGMA journal_mode=WAL;"
                        "PRAGMA synchronous=NORMAL;"
                        "PRAGMA busy_timeout=5000;", error) ||
      !store_migrate (db, error))
    {
      sqlite3_close (db);
      return NULL;
    }

  LogflStore *s = g_new0 (LogflStore, 1);
  s->db = db;
  return s;
}

void
logfl_store_close (LogflStore *s)
{
  if (!s)
    return;
  sqlite3_close (s->db);
  g_free (s);
}

/* --- CRUD -------------------------------------------------------------- */

gboolean
logfl_store_add (LogflStore *s, LogflQso *q, GError **error)
{
  if (!qso_normalize (q, error))
    return FALSE;

  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (s->db,
        "INSERT INTO qso (" QSO_COLS ") VALUES "
        "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        -1, &st, NULL) != SQLITE_OK)
    return sql_fail (s->db, "insert", error);
  bind_qso (st, 1, q);
  gboolean ok = sqlite3_step (st) == SQLITE_DONE;
  sqlite3_finalize (st);
  if (!ok)
    return sql_fail (s->db, "insert", error);
  q->id = sqlite3_last_insert_rowid (s->db);
  return TRUE;
}

gboolean
logfl_store_update (LogflStore *s, LogflQso *q, GError **error)
{
  if (q->id <= 0)
    {
      g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID,
                   "update needs a stored QSO (id)");
      return FALSE;
    }
  if (!qso_normalize (q, error))
    return FALSE;

  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (s->db,
        "UPDATE qso SET ts=?1, call=?2, band=?3, freq=?4, mode=?5, "
        "submode=?6, rst_sent=?7, rst_rcvd=?8, gridsquare=?9, name=?10, "
        "qth=?11, tx_pwr=?12, comment=?13, qsl_rcvd=?14, qsl_sent=?15, "
        "lotw_qsl_rcvd=?16, lotw_qsl_sent=?17, eqsl_qsl_rcvd=?18, "
        "eqsl_qsl_sent=?19, station_callsign=?20, my_gridsquare=?21, "
        "extras=?22 WHERE id=?23;",
        -1, &st, NULL) != SQLITE_OK)
    return sql_fail (s->db, "update", error);
  bind_qso (st, 1, q);
  sqlite3_bind_int64 (st, QSO_N_COLS + 1, q->id);
  gboolean ok = sqlite3_step (st) == SQLITE_DONE;
  sqlite3_finalize (st);
  if (!ok)
    return sql_fail (s->db, "update", error);
  if (sqlite3_changes (s->db) != 1)
    {
      g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_NOT_FOUND,
                   "no QSO with id %" G_GINT64_FORMAT, q->id);
      return FALSE;
    }
  return TRUE;
}

gboolean
logfl_store_delete (LogflStore *s, gint64 id, GError **error)
{
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (s->db, "DELETE FROM qso WHERE id=?1;",
                          -1, &st, NULL) != SQLITE_OK)
    return sql_fail (s->db, "delete", error);
  sqlite3_bind_int64 (st, 1, id);
  gboolean ok = sqlite3_step (st) == SQLITE_DONE;
  sqlite3_finalize (st);
  if (!ok)
    return sql_fail (s->db, "delete", error);
  if (sqlite3_changes (s->db) != 1)
    {
      g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_NOT_FOUND,
                   "no QSO with id %" G_GINT64_FORMAT, id);
      return FALSE;
    }
  return TRUE;
}

LogflQso *
logfl_store_get (LogflStore *s, gint64 id, GError **error)
{
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (s->db,
                          "SELECT id, " QSO_COLS " FROM qso WHERE id=?1;",
                          -1, &st, NULL) != SQLITE_OK)
    {
      sql_fail (s->db, "get", error);
      return NULL;
    }
  sqlite3_bind_int64 (st, 1, id);
  LogflQso *q = NULL;
  if (sqlite3_step (st) == SQLITE_ROW)
    q = row_to_qso (st);
  sqlite3_finalize (st);
  if (!q)
    g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_NOT_FOUND,
                 "no QSO with id %" G_GINT64_FORMAT, id);
  return q;
}

/* --- queries ------------------------------------------------------------ */

GPtrArray *
logfl_store_list (LogflStore *s, const LogflStoreQuery *query, GError **error)
{
  static const LogflStoreQuery none = { 0 };
  const LogflStoreQuery *q = query ? query : &none;

  char *band = norm_dup (q->band, FALSE);
  char *mode = norm_dup (q->mode, TRUE);

  GString *sql = g_string_new ("SELECT id, " QSO_COLS " FROM qso WHERE 1=1");
  if (q->text && *q->text)
    g_string_append (sql,
        " AND (instr(upper(call), upper(?1))"
        " OR instr(upper(ifnull(name,'')), upper(?1))"
        " OR instr(upper(ifnull(qth,'')), upper(?1))"
        " OR instr(upper(ifnull(comment,'')), upper(?1)))");
  if (band)
    g_string_append (sql, " AND band = ?2");
  if (mode)
    g_string_append (sql, " AND mode = ?3");
  g_string_append (sql, " ORDER BY ts DESC, id DESC");
  if (q->limit || q->offset)
    g_string_append (sql, " LIMIT ?4 OFFSET ?5");

  sqlite3_stmt *st = NULL;
  GPtrArray *out = NULL;
  if (sqlite3_prepare_v2 (s->db, sql->str, -1, &st, NULL) != SQLITE_OK)
    {
      sql_fail (s->db, "list", error);
      goto out;
    }
  if (q->text && *q->text)
    sqlite3_bind_text (st, 1, q->text, -1, SQLITE_TRANSIENT);
  if (band)
    sqlite3_bind_text (st, 2, band, -1, SQLITE_TRANSIENT);
  if (mode)
    sqlite3_bind_text (st, 3, mode, -1, SQLITE_TRANSIENT);
  if (q->limit || q->offset)
    {
      sqlite3_bind_int64 (st, 4, q->limit ? (gint64) q->limit : -1);
      sqlite3_bind_int64 (st, 5, q->offset);
    }

  out = g_ptr_array_new_with_free_func ((GDestroyNotify) logfl_qso_free);
  int rc;
  while ((rc = sqlite3_step (st)) == SQLITE_ROW)
    g_ptr_array_add (out, row_to_qso (st));
  if (rc != SQLITE_DONE)
    {
      sql_fail (s->db, "list", error);
      g_clear_pointer (&out, g_ptr_array_unref);
    }

out:
  sqlite3_finalize (st);
  g_string_free (sql, TRUE);
  g_free (band);
  g_free (mode);
  return out;
}

gboolean
logfl_store_worked_b4 (LogflStore *s, const char *call,
                       const char *band, const char *mode,
                       LogflWorkedB4 *out, GError **error)
{
  memset (out, 0, sizeof *out);
  char *ncall = norm_dup (call, TRUE);
  char *nband = norm_dup (band, FALSE);
  char *nmode = norm_dup (mode, TRUE);
  gboolean ok = FALSE;

  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (s->db,
        "SELECT count(*),"
        " ifnull(sum(band = ?2), 0),"
        " ifnull(sum(band = ?2 AND mode = ?3), 0),"
        " ifnull(max(ts), 0)"
        " FROM qso WHERE call = ?1;",
        -1, &st, NULL) != SQLITE_OK)
    {
      sql_fail (s->db, "worked-b4", error);
      goto out;
    }
  bind_str (st, 1, ncall);
  bind_str (st, 2, nband);
  bind_str (st, 3, nmode);
  if (sqlite3_step (st) == SQLITE_ROW)
    {
      out->n_total = (guint) sqlite3_column_int64 (st, 0);
      out->n_band = (guint) sqlite3_column_int64 (st, 1);
      out->n_band_mode = (guint) sqlite3_column_int64 (st, 2);
      out->last_ts = sqlite3_column_int64 (st, 3);
      ok = TRUE;
    }
  else
    sql_fail (s->db, "worked-b4", error);

out:
  sqlite3_finalize (st);
  g_free (ncall);
  g_free (nband);
  g_free (nmode);
  return ok;
}

gboolean
logfl_store_dup_check (LogflStore *s, const char *call,
                       const char *band, const char *mode,
                       gint64 ts, guint window_s,
                       gboolean *is_dup, GError **error)
{
  *is_dup = FALSE;
  char *ncall = norm_dup (call, TRUE);
  char *nband = norm_dup (band, FALSE);
  char *nmode = norm_dup (mode, TRUE);
  gboolean ok = FALSE;

  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (s->db,
        "SELECT EXISTS (SELECT 1 FROM qso WHERE call=?1 AND band=?2"
        " AND mode=?3 AND ts BETWEEN ?4 AND ?5);",
        -1, &st, NULL) != SQLITE_OK)
    {
      sql_fail (s->db, "dup-check", error);
      goto out;
    }
  bind_str (st, 1, ncall);
  bind_str (st, 2, nband);
  bind_str (st, 3, nmode);
  sqlite3_bind_int64 (st, 4, ts - (gint64) window_s);
  sqlite3_bind_int64 (st, 5, ts + (gint64) window_s);
  if (sqlite3_step (st) == SQLITE_ROW)
    {
      *is_dup = sqlite3_column_int (st, 0) != 0;
      ok = TRUE;
    }
  else
    sql_fail (s->db, "dup-check", error);

out:
  sqlite3_finalize (st);
  g_free (ncall);
  g_free (nband);
  g_free (nmode);
  return ok;
}

gboolean
logfl_store_stats (LogflStore *s, LogflStoreStats *out, GError **error)
{
  memset (out, 0, sizeof *out);
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2 (s->db,
        "SELECT count(*), count(DISTINCT call),"
        " ifnull(min(ts), 0), ifnull(max(ts), 0) FROM qso;",
        -1, &st, NULL) != SQLITE_OK)
    return sql_fail (s->db, "stats", error);
  gboolean ok = sqlite3_step (st) == SQLITE_ROW;
  if (ok)
    {
      out->n_qso = (guint) sqlite3_column_int64 (st, 0);
      out->n_calls = (guint) sqlite3_column_int64 (st, 1);
      out->first_ts = sqlite3_column_int64 (st, 2);
      out->last_ts = sqlite3_column_int64 (st, 3);
    }
  sqlite3_finalize (st);
  return ok ? TRUE : sql_fail (s->db, "stats", error);
}

/* --- transactions ------------------------------------------------------- */

gboolean
logfl_store_tx_begin (LogflStore *s, GError **error)
{
  return exec_simple (s->db, "BEGIN IMMEDIATE;", error);
}

gboolean
logfl_store_tx_commit (LogflStore *s, GError **error)
{
  return exec_simple (s->db, "COMMIT;", error);
}

gboolean
logfl_store_tx_rollback (LogflStore *s, GError **error)
{
  return exec_simple (s->db, "ROLLBACK;", error);
}
