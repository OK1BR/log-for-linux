/* store_test.c — M1 gate (log-store-test): schema migration from an empty
 * and a v1 file, CRUD round-trip, normalization, dup/worked-B4 edge cases,
 * list filters and a 50k-QSO volume pass. Headless (docs/SCOPE.md).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "log_store.h"

#include <glib/gstdio.h>

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

static void
test_migrate_empty_and_reopen (void)
{
  GError *err = NULL;
  char *dir = g_dir_make_tmp ("logfl-test-XXXXXX", &err);
  g_assert_no_error (err);
  char *path = g_build_filename (dir, "log.db", NULL);

  /* Fresh file: migration 0 → 1 runs, a QSO lands. */
  LogflStore *s = logfl_store_open (path, &err);
  g_assert_no_error (err);
  LogflQso *q = mk_qso ("OK1ABC", "40m", "CW", 1700000000);
  g_assert_true (logfl_store_add (s, q, &err));
  g_assert_no_error (err);
  logfl_qso_free (q);
  logfl_store_close (s);

  /* Reopen the now-v1 file: no migration, data intact. */
  s = logfl_store_open (path, &err);
  g_assert_no_error (err);
  LogflStoreStats st;
  g_assert_true (logfl_store_stats (s, &st, &err));
  g_assert_cmpuint (st.n_qso, ==, 1);
  logfl_store_close (s);

  g_assert_cmpint (g_unlink (path), ==, 0);
  /* WAL sidecars may or may not exist — remove-if-there. */
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
test_open_bad_path (void)
{
  GError *err = NULL;
  LogflStore *s = logfl_store_open ("/nonexistent-dir/log.db", &err);
  g_assert_null (s);
  g_assert_error (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_OPEN);
  g_clear_error (&err);
}

static void
test_crud_roundtrip (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();

  LogflQso *q = mk_qso ("OK1ABC", "40m", "CW", 1700000000);
  q->freq = 7.0123;
  q->submode = g_strdup ("PCW");
  q->rst_sent = g_strdup ("599");
  q->rst_rcvd = g_strdup ("579");
  q->gridsquare = g_strdup ("JO70");
  q->name = g_strdup ("Pavel");
  q->qth = g_strdup ("Praha");
  q->tx_pwr = 100;
  q->comment = g_strdup ("first sked");
  q->qsl_rcvd = g_strdup ("N");
  q->qsl_sent = g_strdup ("Y");
  q->lotw_qsl_rcvd = g_strdup ("N");
  q->lotw_qsl_sent = g_strdup ("Y");
  q->eqsl_qsl_rcvd = g_strdup ("N");
  q->eqsl_qsl_sent = g_strdup ("N");
  q->station_callsign = g_strdup ("OK1BR");
  q->my_gridsquare = g_strdup ("JO60");
  q->extras = g_strdup ("<CONTEST_ID:4>TEST");

  g_assert_true (logfl_store_add (s, q, &err));
  g_assert_no_error (err);
  g_assert_cmpint (q->id, >, 0);

  LogflQso *r = logfl_store_get (s, q->id, &err);
  g_assert_no_error (err);
  g_assert_cmpint (r->ts, ==, q->ts);
  g_assert_cmpstr (r->call, ==, "OK1ABC");
  g_assert_cmpstr (r->band, ==, "40m");
  g_assert_cmpfloat (r->freq, ==, 7.0123);
  g_assert_cmpstr (r->mode, ==, "CW");
  g_assert_cmpstr (r->submode, ==, "PCW");
  g_assert_cmpstr (r->rst_sent, ==, "599");
  g_assert_cmpstr (r->rst_rcvd, ==, "579");
  g_assert_cmpstr (r->gridsquare, ==, "JO70");
  g_assert_cmpstr (r->name, ==, "Pavel");
  g_assert_cmpstr (r->qth, ==, "Praha");
  g_assert_cmpfloat (r->tx_pwr, ==, 100);
  g_assert_cmpstr (r->comment, ==, "first sked");
  g_assert_cmpstr (r->qsl_rcvd, ==, "N");
  g_assert_cmpstr (r->qsl_sent, ==, "Y");
  g_assert_cmpstr (r->lotw_qsl_rcvd, ==, "N");
  g_assert_cmpstr (r->lotw_qsl_sent, ==, "Y");
  g_assert_cmpstr (r->eqsl_qsl_rcvd, ==, "N");
  g_assert_cmpstr (r->eqsl_qsl_sent, ==, "N");
  g_assert_cmpstr (r->station_callsign, ==, "OK1BR");
  g_assert_cmpstr (r->my_gridsquare, ==, "JO60");
  g_assert_cmpstr (r->extras, ==, "<CONTEST_ID:4>TEST");
  logfl_qso_free (r);

  /* Update. */
  g_free (q->name);
  q->name = g_strdup ("Pavel Novak");
  q->freq = 7.0250;
  g_assert_true (logfl_store_update (s, q, &err));
  g_assert_no_error (err);
  r = logfl_store_get (s, q->id, &err);
  g_assert_cmpstr (r->name, ==, "Pavel Novak");
  g_assert_cmpfloat (r->freq, ==, 7.0250);
  logfl_qso_free (r);

  /* Delete; a second delete and a get must say NOT_FOUND. */
  g_assert_true (logfl_store_delete (s, q->id, &err));
  g_assert_null (logfl_store_get (s, q->id, &err));
  g_assert_error (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_NOT_FOUND);
  g_clear_error (&err);
  g_assert_false (logfl_store_delete (s, q->id, &err));
  g_assert_error (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_NOT_FOUND);
  g_clear_error (&err);

  logfl_qso_free (q);
  logfl_store_close (s);
}

static void
test_normalize_and_invalid (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();

  LogflQso *q = mk_qso ("  ok1abc ", "40M", "cw", 1700000000);
  g_assert_true (logfl_store_add (s, q, &err));
  g_assert_cmpstr (q->call, ==, "OK1ABC");
  g_assert_cmpstr (q->band, ==, "40m");
  g_assert_cmpstr (q->mode, ==, "CW");
  logfl_qso_free (q);

  /* Missing call → INVALID, nothing stored. */
  q = mk_qso ("", "40m", "CW", 1700000000);
  g_assert_false (logfl_store_add (s, q, &err));
  g_assert_error (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID);
  g_clear_error (&err);
  g_assert_cmpint (q->id, ==, 0);
  logfl_qso_free (q);

  LogflStoreStats st;
  g_assert_true (logfl_store_stats (s, &st, &err));
  g_assert_cmpuint (st.n_qso, ==, 1);
  logfl_store_close (s);
}

static void
test_dup_check (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  const gint64 T = 1700000000;

  LogflQso *q = mk_qso ("OK1ABC", "40m", "CW", T);
  g_assert_true (logfl_store_add (s, q, &err));
  logfl_qso_free (q);

  gboolean dup;
  /* Inside the window — case-insensitive on inputs. */
  g_assert_true (logfl_store_dup_check (s, "ok1abc", "40M", "cw",
                                        T + 100, 300, &dup, &err));
  g_assert_true (dup);
  /* Outside the window. */
  g_assert_true (logfl_store_dup_check (s, "OK1ABC", "40m", "CW",
                                        T + 400, 300, &dup, &err));
  g_assert_false (dup);
  /* Window edge is inclusive. */
  g_assert_true (logfl_store_dup_check (s, "OK1ABC", "40m", "CW",
                                        T + 300, 300, &dup, &err));
  g_assert_true (dup);
  /* Other band / other mode are no dup. */
  g_assert_true (logfl_store_dup_check (s, "OK1ABC", "20m", "CW",
                                        T, 300, &dup, &err));
  g_assert_false (dup);
  g_assert_true (logfl_store_dup_check (s, "OK1ABC", "40m", "FT8",
                                        T, 300, &dup, &err));
  g_assert_false (dup);

  logfl_store_close (s);
}

static void
test_worked_b4 (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  const gint64 T = 1700000000;
  const char *specs[][2] = { { "40m", "CW" }, { "20m", "CW" },
                             { "40m", "FT8" } };

  for (gsize i = 0; i < G_N_ELEMENTS (specs); i++)
    {
      LogflQso *q = mk_qso ("OK1ABC", specs[i][0], specs[i][1],
                            T + (gint64) i * 3600);
      g_assert_true (logfl_store_add (s, q, &err));
      logfl_qso_free (q);
    }

  LogflWorkedB4 wb;
  g_assert_true (logfl_store_worked_b4 (s, "ok1abc", "40m", "cw", &wb, &err));
  g_assert_cmpuint (wb.n_total, ==, 3);
  g_assert_cmpuint (wb.n_band, ==, 2);
  g_assert_cmpuint (wb.n_band_mode, ==, 1);
  g_assert_cmpint (wb.last_ts, ==, T + 2 * 3600);

  /* Never worked. */
  g_assert_true (logfl_store_worked_b4 (s, "DL1XYZ", "40m", "CW", &wb, &err));
  g_assert_cmpuint (wb.n_total, ==, 0);
  g_assert_cmpint (wb.last_ts, ==, 0);

  /* band/mode optional. */
  g_assert_true (logfl_store_worked_b4 (s, "OK1ABC", NULL, NULL, &wb, &err));
  g_assert_cmpuint (wb.n_total, ==, 3);
  g_assert_cmpuint (wb.n_band, ==, 0);

  logfl_store_close (s);
}

static void
test_list_filters (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  const gint64 T = 1700000000;

  LogflQso *q;
  q = mk_qso ("OK1ABC", "40m", "CW", T + 10);
  q->name = g_strdup ("Pavel");
  g_assert_true (logfl_store_add (s, q, &err));
  logfl_qso_free (q);
  q = mk_qso ("DL2XYZ", "40m", "SSB", T + 20);
  q->qth = g_strdup ("Berlin");
  g_assert_true (logfl_store_add (s, q, &err));
  logfl_qso_free (q);
  q = mk_qso ("G4AAA", "20m", "CW", T + 30);
  q->comment = g_strdup ("great signal from Pavel's rig");
  g_assert_true (logfl_store_add (s, q, &err));
  logfl_qso_free (q);

  /* No filter: newest first. */
  GPtrArray *l = logfl_store_list (s, NULL, &err);
  g_assert_no_error (err);
  g_assert_cmpuint (l->len, ==, 3);
  g_assert_cmpstr (((LogflQso *) l->pdata[0])->call, ==, "G4AAA");
  g_assert_cmpstr (((LogflQso *) l->pdata[2])->call, ==, "OK1ABC");
  g_ptr_array_unref (l);

  /* Text hits call, name, qth and comment, case-insensitively. */
  LogflStoreQuery f = { .text = "pavel" };
  l = logfl_store_list (s, &f, &err);
  g_assert_cmpuint (l->len, ==, 2);   /* name Pavel + comment …Pavel's… */
  g_ptr_array_unref (l);
  f.text = "berlin";
  l = logfl_store_list (s, &f, &err);
  g_assert_cmpuint (l->len, ==, 1);
  g_assert_cmpstr (((LogflQso *) l->pdata[0])->call, ==, "DL2XYZ");
  g_ptr_array_unref (l);

  /* Band + mode filters, normalized inputs. */
  LogflStoreQuery fb = { .band = "40M" };
  l = logfl_store_list (s, &fb, &err);
  g_assert_cmpuint (l->len, ==, 2);
  g_ptr_array_unref (l);
  LogflStoreQuery fbm = { .band = "40m", .mode = "cw" };
  l = logfl_store_list (s, &fbm, &err);
  g_assert_cmpuint (l->len, ==, 1);
  g_assert_cmpstr (((LogflQso *) l->pdata[0])->call, ==, "OK1ABC");
  g_ptr_array_unref (l);

  /* limit/offset paging. */
  LogflStoreQuery fp = { .limit = 2 };
  l = logfl_store_list (s, &fp, &err);
  g_assert_cmpuint (l->len, ==, 2);
  g_ptr_array_unref (l);
  fp.offset = 2;
  l = logfl_store_list (s, &fp, &err);
  g_assert_cmpuint (l->len, ==, 1);
  g_assert_cmpstr (((LogflQso *) l->pdata[0])->call, ==, "OK1ABC");
  g_ptr_array_unref (l);

  logfl_store_close (s);
}

static void
test_volume_50k (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  const gint64 T = 1600000000;
  static const char *bands[] = { "160m", "80m", "40m", "20m", "15m", "10m" };
  static const char *modes[] = { "CW", "SSB", "FT8" };
  const guint N = 50000;

  GRand *rnd = g_rand_new_with_seed (73);
  gint64 t0 = g_get_monotonic_time ();

  g_assert_true (logfl_store_tx_begin (s, &err));
  for (guint i = 0; i < N; i++)
    {
      LogflQso *q;
      if (i % 1000 == 0)
        /* A known repeat customer, 50 times across the log. */
        q = mk_qso ("DL1TEST", "40m", "CW", T + (gint64) i * 60);
      else
        {
          char call[16];
          g_snprintf (call, sizeof call, "OK%uQ%c%c",
                      1 + g_rand_int_range (rnd, 0, 9),
                      'A' + g_rand_int_range (rnd, 0, 26),
                      'A' + g_rand_int_range (rnd, 0, 26));
          q = mk_qso (call,
                      bands[g_rand_int_range (rnd, 0, G_N_ELEMENTS (bands))],
                      modes[g_rand_int_range (rnd, 0, G_N_ELEMENTS (modes))],
                      T + (gint64) i * 60);
        }
      g_assert_true (logfl_store_add (s, q, &err));
      logfl_qso_free (q);
    }
  g_assert_true (logfl_store_tx_commit (s, &err));
  gint64 t_insert = g_get_monotonic_time () - t0;

  LogflStoreStats st;
  g_assert_true (logfl_store_stats (s, &st, &err));
  g_assert_cmpuint (st.n_qso, ==, N);
  g_assert_cmpint (st.first_ts, ==, T);
  g_assert_cmpint (st.last_ts, ==, T + (gint64) (N - 1) * 60);

  t0 = g_get_monotonic_time ();
  LogflWorkedB4 wb;
  g_assert_true (logfl_store_worked_b4 (s, "DL1TEST", "40m", "CW", &wb, &err));
  g_assert_cmpuint (wb.n_total, ==, 50);
  g_assert_cmpuint (wb.n_band_mode, ==, 50);
  gboolean dup;
  g_assert_true (logfl_store_dup_check (s, "DL1TEST", "40m", "CW",
                                        T + 60, 300, &dup, &err));
  g_assert_true (dup);
  LogflStoreQuery fp = { .limit = 20 };
  GPtrArray *l = logfl_store_list (s, &fp, &err);
  g_assert_cmpuint (l->len, ==, 20);
  g_ptr_array_unref (l);
  gint64 t_query = g_get_monotonic_time () - t0;

  g_test_message ("50k inserts: %.1f ms; worked-b4 + dup + list-20: %.1f ms",
                  t_insert / 1000.0, t_query / 1000.0);

  g_rand_free (rnd);
  logfl_store_close (s);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/store/migrate-empty-and-reopen",
                   test_migrate_empty_and_reopen);
  g_test_add_func ("/store/open-bad-path", test_open_bad_path);
  g_test_add_func ("/store/crud-roundtrip", test_crud_roundtrip);
  g_test_add_func ("/store/normalize-and-invalid", test_normalize_and_invalid);
  g_test_add_func ("/store/dup-check", test_dup_check);
  g_test_add_func ("/store/worked-b4", test_worked_b4);
  g_test_add_func ("/store/list-filters", test_list_filters);
  g_test_add_func ("/store/volume-50k", test_volume_50k);
  return g_test_run ();
}
