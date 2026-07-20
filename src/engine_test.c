/* engine_test.c — M0 gate (log-engine-test): the engine builds headless,
 * reports its identity and the sqlite3 dependency works end to end.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "engine.h"

static void
test_version (void)
{
  g_assert_cmpstr (logfl_engine_version (), ==, LOGFL_VERSION);
}

static void
test_sqlite_version (void)
{
  const char *v = logfl_engine_sqlite_version ();
  g_assert_nonnull (v);
  g_assert_true (g_str_has_prefix (v, "3."));
}

static void
test_selfcheck (void)
{
  GError *error = NULL;
  g_assert_true (logfl_engine_selfcheck (&error));
  g_assert_no_error (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/engine/version", test_version);
  g_test_add_func ("/engine/sqlite-version", test_sqlite_version);
  g_test_add_func ("/engine/selfcheck", test_selfcheck);
  return g_test_run ();
}
