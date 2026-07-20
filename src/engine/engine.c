/* engine.c — logbook engine identity + selfcheck (see engine.h).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "engine.h"

#include <sqlite3.h>

#ifndef LOGFL_VERSION
#define LOGFL_VERSION "0.0.0"
#endif

GQuark
logfl_engine_error_quark (void)
{
  return g_quark_from_static_string ("logfl-engine-error");
}

const char *
logfl_engine_version (void)
{
  return LOGFL_VERSION;
}

const char *
logfl_engine_sqlite_version (void)
{
  return sqlite3_libversion ();
}

static gboolean
selfcheck_fail (sqlite3 *db, const char *stage, GError **error)
{
  g_set_error (error, LOGFL_ENGINE_ERROR, LOGFL_ENGINE_ERROR_SQLITE,
               "sqlite selfcheck failed at %s: %s",
               stage, db ? sqlite3_errmsg (db) : "(open)");
  if (db)
    sqlite3_close (db);
  return FALSE;
}

gboolean
logfl_engine_selfcheck (GError **error)
{
  sqlite3 *db = NULL;

  if (sqlite3_open (":memory:", &db) != SQLITE_OK)
    return selfcheck_fail (db, "open", error);

  if (sqlite3_exec (db,
                    "CREATE TABLE t (x INTEGER); INSERT INTO t VALUES (73);",
                    NULL, NULL, NULL) != SQLITE_OK)
    return selfcheck_fail (db, "exec", error);

  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2 (db, "SELECT x FROM t;", -1, &stmt, NULL) != SQLITE_OK)
    return selfcheck_fail (db, "prepare", error);

  gboolean ok = sqlite3_step (stmt) == SQLITE_ROW &&
                sqlite3_column_int (stmt, 0) == 73;
  sqlite3_finalize (stmt);

  if (!ok)
    return selfcheck_fail (db, "readback", error);

  sqlite3_close (db);
  return TRUE;
}
