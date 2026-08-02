/* contest_test.c — contest gate (log-contest-test): exchange-definition
 * parse/serialize round trip, presets, exchange routing onto a QSO, the
 * v1 → v2 store migration, contest CRUD, serial/dup/scoping queries,
 * delete semantics and the contest fields' ADIF round trip. Headless
 * (docs/SCOPE.md).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "adif.h"
#include "contest.h"
#include "log_store.h"

#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>

static LogflQso *
mk_qso (const char *call, const char *band, const char *mode, gint64 ts)
{
  LogflQso *q = logfl_qso_new ();
  q->call = g_strdup (call);
  q->band = g_strdup (band);
  q->mode = g_strdup (mode);
  q->ts = ts;
  return q;
}

static LogflStore *
mem_store (void)
{
  GError *err = NULL;
  LogflStore *s = logfl_store_open (":memory:", &err);
  g_assert_no_error (err);
  g_assert_nonnull (s);
  return s;
}

static LogflContest *
mk_contest (LogflStore *s, const char *name, const char *adif_id,
            const char *exch_def)
{
  GError *err = NULL;
  LogflContest *c = logfl_contest_new ();
  c->name = g_strdup (name);
  c->adif_id = g_strdup (adif_id);
  c->exch_def = g_strdup (exch_def);
  g_assert_true (logfl_store_contest_add (s, c, &err));
  g_assert_no_error (err);
  g_assert_cmpint (c->id, >, 0);
  return c;
}

/* Adds a contest QSO in one go. */
static void
add_cqso (LogflStore *s, gint64 contest, const char *call, const char *band,
          const char *mode, gint64 ts, gint64 stx)
{
  GError *err = NULL;
  LogflQso *q = mk_qso (call, band, mode, ts);
  q->contest_ref = contest;
  q->stx = stx;
  g_assert_true (logfl_store_add (s, q, &err));
  g_assert_no_error (err);
  logfl_qso_free (q);
}

static guint
count_scoped (LogflStore *s, gint64 contest)
{
  GError *err = NULL;
  LogflStoreQuery f = { .contest = contest };
  GPtrArray *l = logfl_store_list (s, &f, &err);
  g_assert_no_error (err);
  guint n = l->len;
  g_ptr_array_unref (l);
  return n;
}

/* --- exchange definitions ----------------------------------------------- */

static const char *DEF_TWO_FIELDS =
  "[exchange]\ntx_serial=true\nfields=nr;okres;\n"
  "[field:nr]\nlabel=Nr\ntype=serial\nrequired=true\n"
  "[field:okres]\nlabel=Okres\ntype=text\nrequired=false\n";

static void
test_exch_def_roundtrip (void)
{
  GError *err = NULL;
  LogflExchDef *def = logfl_exch_def_parse (DEF_TWO_FIELDS, &err);
  g_assert_no_error (err);
  g_assert_nonnull (def);
  g_assert_true (def->tx_serial);
  g_assert_cmpuint (def->fields->len, ==, 2);

  LogflExchField *f = def->fields->pdata[0];
  g_assert_cmpstr (f->key, ==, "nr");
  g_assert_cmpstr (f->label, ==, "Nr");
  g_assert_cmpint (f->type, ==, LOGFL_EXCH_SERIAL);
  g_assert_cmpstr (f->adif_num, ==, "SRX");
  g_assert_cmpstr (f->adif_text, ==, "SRX_STRING");
  g_assert_true (f->required);
  f = def->fields->pdata[1];
  g_assert_cmpstr (f->key, ==, "okres");
  g_assert_cmpint (f->type, ==, LOGFL_EXCH_TEXT);
  g_assert_false (f->required);

  /* Serialize → parse → identical shape. */
  char *text = logfl_exch_def_serialize (def);
  LogflExchDef *back = logfl_exch_def_parse (text, &err);
  g_assert_no_error (err);
  g_assert_true (back->tx_serial);
  g_assert_cmpuint (back->fields->len, ==, 2);
  for (guint i = 0; i < 2; i++)
    {
      LogflExchField *a = def->fields->pdata[i];
      LogflExchField *b = back->fields->pdata[i];
      g_assert_cmpstr (a->key, ==, b->key);
      g_assert_cmpstr (a->label, ==, b->label);
      g_assert_cmpint (a->type, ==, b->type);
      g_assert_cmpstr (a->adif_num, ==, b->adif_num);
      g_assert_cmpstr (a->adif_text, ==, b->adif_text);
      g_assert_cmpint (a->required, ==, b->required);
    }
  g_free (text);
  logfl_exch_def_free (back);
  logfl_exch_def_free (def);
}

static void
test_exch_def_errors (void)
{
  GError *err = NULL;

  /* No [exchange] group. */
  g_assert_null (logfl_exch_def_parse ("", &err));
  g_assert_error (err, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE);
  g_clear_error (&err);

  /* Unknown field type. */
  g_assert_null (logfl_exch_def_parse (
      "[exchange]\nfields=a;\n[field:a]\ntype=weird\n", &err));
  g_assert_error (err, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE);
  g_clear_error (&err);

  /* Bad ADIF target name. */
  g_assert_null (logfl_exch_def_parse (
      "[exchange]\nfields=a;\n[field:a]\nadif_num=b@d\n", &err));
  g_assert_error (err, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE);
  g_clear_error (&err);

  /* Not a keyfile at all — GKeyFile's own error is fine, just no crash. */
  g_assert_null (logfl_exch_def_parse ("\1\2 garbage", &err));
  g_assert_nonnull (err);
  g_clear_error (&err);
}

static void
test_presets (void)
{
  guint n = 0;
  const LogflContestPreset *p = logfl_contest_presets (&n);
  g_assert_cmpuint (n, >=, 5);

  for (guint i = 0; i < n; i++)
    {
      GError *err = NULL;
      g_assert_nonnull (p[i].name);
      LogflExchDef *def = logfl_exch_def_parse (p[i].exch_def, &err);
      g_assert_no_error (err);
      g_assert_cmpuint (def->fields->len, >=, 1);
      logfl_exch_def_free (def);
    }

  /* CQ WW routes the zone into CQZ. */
  g_assert_cmpstr (p[0].name, ==, "CQ WW");
  GError *err = NULL;
  LogflExchDef *def = logfl_exch_def_parse (p[0].exch_def, &err);
  g_assert_no_error (err);
  LogflExchField *f = def->fields->pdata[0];
  g_assert_cmpstr (f->adif_num, ==, "CQZ");
  g_assert_false (def->tx_serial);
  logfl_exch_def_free (def);

  /* EUHFC: the year of first licence is TEXT — "01" (2001) must keep its
   * leading zero instead of turning into a serial. */
  gboolean euhfc = FALSE;
  for (guint i = 0; i < n; i++)
    if (g_strcmp0 (p[i].name, "EUHFC") == 0)
      {
        euhfc = TRUE;
        g_assert_cmpstr (p[i].adif_id, ==, "EU-HF");
        def = logfl_exch_def_parse (p[i].exch_def, &err);
        g_assert_no_error (err);
        f = def->fields->pdata[0];
        g_assert_cmpint (f->type, ==, LOGFL_EXCH_TEXT);
        g_assert_false (def->tx_serial);
        logfl_exch_def_free (def);
      }
  g_assert_true (euhfc);
}

static void
test_exch_apply (void)
{
  GError *err = NULL;
  LogflExchDef *def = logfl_exch_def_parse (
      "[exchange]\ntx_serial=true\nfields=zone;nr;dist;\n"
      "[field:zone]\nlabel=Zone\ntype=number\nadif_num=CQZ\n"
      "[field:nr]\nlabel=Nr\ntype=serial\n"
      "[field:dist]\nlabel=District\ntype=text\n", &err);
  g_assert_no_error (err);

  /* Full exchange: number → CQZ tag in extras AND mirrored into the string
   * column (Cabrillo + the table read only the exchange columns), serial →
   * srx, text → string; sent side gets serial + uppercased static exch. */
  LogflQso *q = logfl_qso_new ();
  const char *vals[] = { "15", "007", "apa" };
  logfl_exch_apply (def, vals, 3, "jn69", 12, q);
  g_assert_cmpint (q->stx, ==, 12);
  g_assert_cmpstr (q->stx_string, ==, "JN69");
  g_assert_cmpint (q->srx, ==, 7);
  g_assert_cmpstr (q->srx_string, ==, "15 APA");
  g_assert_nonnull (q->extras);
  g_assert_nonnull (strstr (q->extras, "<CQZ:2>15"));
  logfl_qso_free (q);

  /* A text field keeps digits textual (district "01" is not a serial);
   * empty/NULL values are skipped. */
  q = logfl_qso_new ();
  const char *vals2[] = { NULL, "", "01" };
  logfl_exch_apply (def, vals2, 3, NULL, 0, q);
  g_assert_cmpint (q->srx, ==, 0);
  g_assert_cmpstr (q->srx_string, ==, "01");
  g_assert_cmpint (q->stx, ==, 0);
  g_assert_null (q->stx_string);
  logfl_qso_free (q);
  logfl_exch_def_free (def);

  /* auto: digits → adif_num, text → adif_text (OK/OM DX both-ways). */
  def = logfl_exch_def_parse (
      "[exchange]\nfields=exch;\n[field:exch]\ntype=auto\n", &err);
  g_assert_no_error (err);
  q = logfl_qso_new ();
  const char *dx[] = { "042" };
  logfl_exch_apply (def, dx, 1, NULL, 0, q);
  g_assert_cmpint (q->srx, ==, 42);
  g_assert_null (q->srx_string);
  logfl_qso_free (q);
  q = logfl_qso_new ();
  const char *ok[] = { "apa" };
  logfl_exch_apply (def, ok, 1, NULL, 0, q);
  g_assert_cmpint (q->srx, ==, 0);
  g_assert_cmpstr (q->srx_string, ==, "APA");
  logfl_qso_free (q);
  logfl_exch_def_free (def);

  char *nr = logfl_exch_serial_format (7);
  g_assert_cmpstr (nr, ==, "007");
  g_free (nr);
  nr = logfl_exch_serial_format (1234);
  g_assert_cmpstr (nr, ==, "1234");
  g_free (nr);
}

/* --- store -------------------------------------------------------------- */

static void
test_contest_crud (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  guint n_presets = 0;
  const LogflContestPreset *p = logfl_contest_presets (&n_presets);

  LogflContest *c = mk_contest (s, "  CQ WW CW 2026 ", "CQ-WW-CW",
                                p[0].exch_def);
  g_assert_cmpstr (c->name, ==, "CQ WW CW 2026");   /* stripped */
  g_assert_cmpint (c->created, >, 0);

  LogflContest *r = logfl_store_contest_get (s, c->id, &err);
  g_assert_no_error (err);
  g_assert_cmpstr (r->name, ==, "CQ WW CW 2026");
  g_assert_cmpstr (r->adif_id, ==, "CQ-WW-CW");
  g_assert_cmpstr (r->exch_def, ==, p[0].exch_def);
  g_assert_cmpint (r->created, ==, c->created);
  logfl_contest_free (r);

  g_free (c->my_exch);
  c->my_exch = g_strdup ("15");
  g_assert_true (logfl_store_contest_update (s, c, &err));
  r = logfl_store_contest_get (s, c->id, &err);
  g_assert_cmpstr (r->my_exch, ==, "15");
  logfl_contest_free (r);

  GPtrArray *l = logfl_store_contest_list (s, &err);
  g_assert_no_error (err);
  g_assert_cmpuint (l->len, ==, 1);
  g_ptr_array_unref (l);

  /* Missing name/def → INVALID, nothing stored. */
  LogflContest *bad = logfl_contest_new ();
  bad->name = g_strdup ("x");
  g_assert_false (logfl_store_contest_add (s, bad, &err));
  g_assert_error (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID);
  g_clear_error (&err);
  logfl_contest_free (bad);

  /* Unknown id → NOT_FOUND. */
  g_assert_null (logfl_store_contest_get (s, 999, &err));
  g_assert_error (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_NOT_FOUND);
  g_clear_error (&err);
  g_assert_false (logfl_store_contest_delete (s, 999, FALSE, NULL, &err));
  g_assert_error (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_NOT_FOUND);
  g_clear_error (&err);

  logfl_contest_free (c);
  logfl_store_close (s);
}

static void
test_migrate_v1 (void)
{
  GError *err = NULL;
  char *dir = g_dir_make_tmp ("logfl-test-XXXXXX", &err);
  g_assert_no_error (err);
  char *path = g_build_filename (dir, "log.db", NULL);

  /* Hand-build a v1 file the way the app wrote them before contests. */
  sqlite3 *db = NULL;
  g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (db,
      "CREATE TABLE qso ("
      "  id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, call TEXT NOT NULL,"
      "  band TEXT NOT NULL, freq REAL, mode TEXT NOT NULL, submode TEXT,"
      "  rst_sent TEXT, rst_rcvd TEXT, gridsquare TEXT, name TEXT, qth TEXT,"
      "  tx_pwr REAL, comment TEXT, qsl_rcvd TEXT, qsl_sent TEXT,"
      "  lotw_qsl_rcvd TEXT, lotw_qsl_sent TEXT, eqsl_qsl_rcvd TEXT,"
      "  eqsl_qsl_sent TEXT, station_callsign TEXT, my_gridsquare TEXT,"
      "  extras TEXT);"
      "CREATE INDEX idx_qso_ts ON qso (ts DESC);"
      "CREATE INDEX idx_qso_call ON qso (call);"
      "CREATE INDEX idx_qso_call_band_mode ON qso (call, band, mode);"
      "PRAGMA user_version = 1;"
      "INSERT INTO qso (ts, call, band, mode)"
      " VALUES (1700000000, 'OK1ABC', '40m', 'CW');",
      NULL, NULL, NULL), ==, SQLITE_OK);
  sqlite3_close (db);

  /* Open through the API: v1 → v2 migration must keep the data. */
  LogflStore *s = logfl_store_open (path, &err);
  g_assert_no_error (err);
  LogflQso *q = logfl_store_get (s, 1, &err);
  g_assert_no_error (err);
  g_assert_cmpstr (q->call, ==, "OK1ABC");
  g_assert_cmpint (q->contest_ref, ==, 0);
  g_assert_cmpint (q->stx, ==, 0);
  g_assert_null (q->srx_string);
  logfl_qso_free (q);

  /* And the contest machinery works in the migrated file. */
  guint np = 0;
  const LogflContestPreset *p = logfl_contest_presets (&np);
  LogflContest *c = mk_contest (s, "Test", NULL, p[1].exch_def);
  add_cqso (s, c->id, "DL1AA", "40m", "CW", 1700000100, 1);
  g_assert_cmpuint (count_scoped (s, LOGFL_QUERY_CONTEST_ALL), ==, 2);
  g_assert_cmpuint (count_scoped (s, LOGFL_QUERY_CONTEST_NONE), ==, 1);
  g_assert_cmpuint (count_scoped (s, c->id), ==, 1);
  logfl_contest_free (c);
  logfl_store_close (s);

  /* Reopen: already v2, no re-migration, data intact. */
  s = logfl_store_open (path, &err);
  g_assert_no_error (err);
  g_assert_cmpuint (count_scoped (s, LOGFL_QUERY_CONTEST_ALL), ==, 2);
  logfl_store_close (s);

  g_assert_cmpint (g_unlink (path), ==, 0);
  char *wal = g_strconcat (path, "-wal", NULL);
  char *shm = g_strconcat (path, "-shm", NULL);
  g_unlink (wal);
  g_unlink (shm);
  g_free (wal);
  g_free (shm);
  g_assert_cmpint (g_rmdir (dir), ==, 0);
  g_free (path);
  g_free (dir);
}

static void
test_serial_dup_scoping (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  const gint64 T = 1700000000;
  guint np = 0;
  const LogflContestPreset *p = logfl_contest_presets (&np);

  LogflContest *a = mk_contest (s, "A", NULL, p[1].exch_def);
  LogflContest *b = mk_contest (s, "B", NULL, p[1].exch_def);

  guint next = 0;
  g_assert_true (logfl_store_serial_next (s, a->id, &next, &err));
  g_assert_cmpuint (next, ==, 1);

  add_cqso (s, a->id, "OK1ABC", "40m", "CW", T + 10, 1);
  add_cqso (s, a->id, "OK1ABC", "20m", "CW", T + 20, 2);
  add_cqso (s, a->id, "DL1AA", "40m", "CW", T + 30, 3);
  add_cqso (s, b->id, "G3XYZ", "40m", "CW", T + 40, 1);
  add_cqso (s, 0, "OK1ABC", "40m", "CW", T + 50, 0);   /* main log */

  g_assert_true (logfl_store_serial_next (s, a->id, &next, &err));
  g_assert_cmpuint (next, ==, 4);
  g_assert_true (logfl_store_serial_next (s, b->id, &next, &err));
  g_assert_cmpuint (next, ==, 2);

  /* Dup is per contest, whole duration, normalized inputs. */
  gboolean dup;
  g_assert_true (logfl_store_contest_dup_check (s, a->id, "ok1abc", "40M",
                                                "cw", &dup, &err));
  g_assert_true (dup);
  g_assert_true (logfl_store_contest_dup_check (s, a->id, "OK1ABC", "15m",
                                                "CW", &dup, &err));
  g_assert_false (dup);
  /* The main-log OK1ABC 40m CW does not pollute contest B. */
  g_assert_true (logfl_store_contest_dup_check (s, b->id, "OK1ABC", "40m",
                                                "CW", &dup, &err));
  g_assert_false (dup);

  /* Scoping: all / main-only / per contest. */
  g_assert_cmpuint (count_scoped (s, LOGFL_QUERY_CONTEST_ALL), ==, 5);
  g_assert_cmpuint (count_scoped (s, LOGFL_QUERY_CONTEST_NONE), ==, 1);
  g_assert_cmpuint (count_scoped (s, a->id), ==, 3);
  g_assert_cmpuint (count_scoped (s, b->id), ==, 1);

  /* Worked-B4 stays global — the whole canonical log answers. */
  LogflWorkedB4 wb;
  g_assert_true (logfl_store_worked_b4 (s, "OK1ABC", "40m", "CW", &wb, &err));
  g_assert_cmpuint (wb.n_total, ==, 3);

  LogflStoreStats st;
  g_assert_true (logfl_store_contest_stats (s, a->id, &st, &err));
  g_assert_cmpuint (st.n_qso, ==, 3);
  g_assert_cmpuint (st.n_calls, ==, 2);
  g_assert_cmpint (st.first_ts, ==, T + 10);
  g_assert_cmpint (st.last_ts, ==, T + 30);

  logfl_contest_free (a);
  logfl_contest_free (b);
  logfl_store_close (s);
}

static void
test_delete_semantics (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  const gint64 T = 1700000000;
  guint np = 0;
  const LogflContestPreset *p = logfl_contest_presets (&np);

  LogflContest *a = mk_contest (s, "A", NULL, p[1].exch_def);
  LogflContest *b = mk_contest (s, "B", NULL, p[1].exch_def);
  add_cqso (s, a->id, "OK1ABC", "40m", "CW", T + 10, 1);
  add_cqso (s, a->id, "DL1AA", "40m", "CW", T + 20, 2);
  add_cqso (s, b->id, "G3XYZ", "40m", "CW", T + 30, 1);
  add_cqso (s, 0, "F5AAA", "40m", "CW", T + 40, 0);

  /* Keep the QSOs: they fall back into the main log. */
  guint n = 0;
  g_assert_true (logfl_store_contest_delete (s, a->id, FALSE, &n, &err));
  g_assert_no_error (err);
  g_assert_cmpuint (n, ==, 2);
  g_assert_null (logfl_store_contest_get (s, a->id, &err));
  g_assert_error (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_NOT_FOUND);
  g_clear_error (&err);
  g_assert_cmpuint (count_scoped (s, LOGFL_QUERY_CONTEST_NONE), ==, 3);
  g_assert_cmpuint (count_scoped (s, LOGFL_QUERY_CONTEST_ALL), ==, 4);

  /* Delete including the QSOs. */
  g_assert_true (logfl_store_contest_delete (s, b->id, TRUE, &n, &err));
  g_assert_cmpuint (n, ==, 1);
  g_assert_cmpuint (count_scoped (s, LOGFL_QUERY_CONTEST_ALL), ==, 3);
  g_assert_cmpuint (count_scoped (s, LOGFL_QUERY_CONTEST_NONE), ==, 3);

  logfl_contest_free (a);
  logfl_contest_free (b);
  logfl_store_close (s);
}

/* --- ADIF --------------------------------------------------------------- */

static void
test_adif_roundtrip (void)
{
  GError *err = NULL;
  LogflStore *s1 = mem_store ();
  guint np = 0;
  const LogflContestPreset *p = logfl_contest_presets (&np);

  LogflContest *c = mk_contest (s1, "CQ WPX CW 2026", "CQ-WPX-CW",
                                p[1].exch_def);
  LogflQso *q = mk_qso ("OK1ABC", "40m", "CW", 1700000000);
  q->rst_sent = g_strdup ("599");
  q->contest_ref = c->id;
  q->stx = 1;
  q->srx = 42;
  g_assert_true (logfl_store_add (s1, q, &err));
  logfl_qso_free (q);
  q = mk_qso ("DL1AA", "40m", "CW", 1700000060);
  q->contest_ref = c->id;
  q->stx = 2;
  q->srx_string = g_strdup ("APA");
  g_assert_true (logfl_store_add (s1, q, &err));
  logfl_qso_free (q);

  /* Export (default query = everything) carries the contest fields. */
  guint n = 0;
  char *data = logfl_adif_export_data (s1, NULL, &n, &err);
  g_assert_no_error (err);
  g_assert_cmpuint (n, ==, 2);
  g_assert_nonnull (strstr (data, "<CONTEST_ID:9>CQ-WPX-CW"));
  g_assert_nonnull (strstr (data, "<STX:1>1"));
  g_assert_nonnull (strstr (data, "<SRX:2>42"));
  g_assert_nonnull (strstr (data, "<SRX_STRING:3>APA"));

  /* Import into a fresh store: columns come back, CONTEST_ID survives in
   * extras (no auto-created contest — deliberate). */
  LogflStore *s2 = mem_store ();
  LogflAdifReport rep;
  g_assert_true (logfl_adif_import_data (s2, data, -1, 0, &rep, &err));
  g_assert_cmpuint (rep.n_imported, ==, 2);
  g_assert_cmpuint (rep.n_bad, ==, 0);

  LogflStoreQuery f = { .text = "OK1ABC" };
  GPtrArray *l = logfl_store_list (s2, &f, &err);
  g_assert_cmpuint (l->len, ==, 1);
  LogflQso *r = l->pdata[0];
  g_assert_cmpint (r->contest_ref, ==, 0);
  g_assert_cmpint (r->stx, ==, 1);
  g_assert_cmpint (r->srx, ==, 42);
  g_assert_nonnull (r->extras);
  g_assert_nonnull (strstr (r->extras, "<CONTEST_ID:9>CQ-WPX-CW"));
  g_ptr_array_unref (l);

  GPtrArray *cl = logfl_store_contest_list (s2, &err);
  g_assert_cmpuint (cl->len, ==, 0);
  g_ptr_array_unref (cl);

  /* Byte stability from here on: export(import(export)) is a fixpoint. */
  char *data2 = logfl_adif_export_data (s2, NULL, NULL, &err);
  LogflStore *s3 = mem_store ();
  g_assert_true (logfl_adif_import_data (s3, data2, -1, 0, &rep, &err));
  char *data3 = logfl_adif_export_data (s3, NULL, NULL, &err);
  g_assert_cmpstr (data2, ==, data3);

  g_free (data);
  g_free (data2);
  g_free (data3);
  logfl_contest_free (c);
  logfl_store_close (s1);
  logfl_store_close (s2);
  logfl_store_close (s3);
}

static void
test_adif_serial_quirks (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();

  /* Leading zeros normalize into the integer column; a non-numeric STX is
   * preserved verbatim in extras instead of being mangled. */
  const char *adi =
    "<CALL:6>OK1ABC<QSO_DATE:8>20260101<TIME_ON:4>1200<BAND:3>40m"
    "<MODE:2>CW<SRX:3>007<STX:5>00x12<EOR>";
  LogflAdifReport rep;
  g_assert_true (logfl_adif_import_data (s, adi, -1, 0, &rep, &err));
  g_assert_cmpuint (rep.n_imported, ==, 1);

  GPtrArray *l = logfl_store_list (s, NULL, &err);
  g_assert_cmpuint (l->len, ==, 1);
  LogflQso *q = l->pdata[0];
  g_assert_cmpint (q->srx, ==, 7);
  g_assert_cmpint (q->stx, ==, 0);
  g_assert_nonnull (q->extras);
  g_assert_nonnull (strstr (q->extras, "<STX:5>00x12"));
  g_ptr_array_unref (l);
  logfl_store_close (s);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/contest/exch-def-roundtrip", test_exch_def_roundtrip);
  g_test_add_func ("/contest/exch-def-errors", test_exch_def_errors);
  g_test_add_func ("/contest/presets", test_presets);
  g_test_add_func ("/contest/exch-apply", test_exch_apply);
  g_test_add_func ("/contest/store-crud", test_contest_crud);
  g_test_add_func ("/contest/migrate-v1", test_migrate_v1);
  g_test_add_func ("/contest/serial-dup-scoping", test_serial_dup_scoping);
  g_test_add_func ("/contest/delete-semantics", test_delete_semantics);
  g_test_add_func ("/contest/adif-roundtrip", test_adif_roundtrip);
  g_test_add_func ("/contest/adif-serial-quirks", test_adif_serial_quirks);
  return g_test_run ();
}
