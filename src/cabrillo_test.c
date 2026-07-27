/* cabrillo_test.c — Cabrillo gate (log-cabrillo-test): header tags, QSO
 * line shape (kHz / generic band freq / VHF designator, mode mapping,
 * RST defaults, serial + text exchanges), chronological order and the
 * required-field errors. Headless (docs/SCOPE.md).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "cabrillo.h"
#include "contest.h"

#include <string.h>

static LogflStore *
mem_store (void)
{
  GError *err = NULL;
  LogflStore *s = logfl_store_open (":memory:", &err);
  g_assert_no_error (err);
  g_assert_nonnull (s);
  return s;
}

static gint64
mk_contest (LogflStore *s)
{
  GError *err = NULL;
  guint n = 0;
  const LogflContestPreset *p = logfl_contest_presets (&n);
  LogflContest *c = logfl_contest_new ();
  c->name = g_strdup ("Test Contest");
  c->exch_def = g_strdup (p[1].exch_def);
  g_assert_true (logfl_store_contest_add (s, c, &err));
  gint64 id = c->id;
  logfl_contest_free (c);
  return id;
}

static void
add_qso (LogflStore *s, gint64 contest, const char *call, const char *band,
         double freq, const char *mode, gint64 ts,
         const char *rst_s, const char *rst_r,
         gint64 stx, const char *stx_str, gint64 srx, const char *srx_str)
{
  GError *err = NULL;
  LogflQso *q = logfl_qso_new ();
  q->call = g_strdup (call);
  q->band = g_strdup (band);
  q->mode = g_strdup (mode);
  q->freq = freq;
  q->ts = ts;
  q->rst_sent = g_strdup (rst_s);
  q->rst_rcvd = g_strdup (rst_r);
  q->contest_ref = contest;
  q->stx = stx;
  q->stx_string = g_strdup (stx_str);
  q->srx = srx;
  q->srx_string = g_strdup (srx_str);
  q->station_callsign = g_strdup ("OK1BR");
  g_assert_true (logfl_store_add (s, q, &err));
  g_assert_no_error (err);
  logfl_qso_free (q);
}

static const LogflCabrilloOpts OPTS = {
  .contest = "EUHFC",
  .callsign = "OK1BR",
  .cat_operator = "SINGLE-OP",
  .cat_band = "ALL",
  .cat_power = "LOW",
  .cat_mode = "MIXED",
  .name = "Richard",
  .location = "DX",
  .grid = "JO60TD",
};

static void
test_header_and_order (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  gint64 c = mk_contest (s);
  const gint64 T = 1754049600;         /* 2025-08-01 12:00:00 UTC */

  /* Inserted newest-first — export must come out chronological. */
  add_qso (s, c, "S50A", "40m", 7.0123, "CW", T + 3600,
           "599", "579", 2, NULL, 0, "63");
  add_qso (s, c, "DL1AA", "20m", 14.205, "SSB", T,
           "59", "59", 1, NULL, 0, "82");
  /* Main-log QSO must not leak into the contest export. */
  add_qso (s, 0, "G3XYZ", "40m", 7.02, "CW", T + 10,
           NULL, NULL, 0, NULL, 0, NULL);

  guint n = 0;
  char *out = logfl_cabrillo_export (s, c, &OPTS, &n, &err);
  g_assert_no_error (err);
  g_assert_cmpuint (n, ==, 2);

  g_assert_true (g_str_has_prefix (out, "START-OF-LOG: 3.0\n"));
  g_assert_nonnull (strstr (out, "CALLSIGN: OK1BR\n"));
  g_assert_nonnull (strstr (out, "CONTEST: EUHFC\n"));
  g_assert_nonnull (strstr (out, "CATEGORY-OPERATOR: SINGLE-OP\n"));
  g_assert_nonnull (strstr (out, "CATEGORY-BAND: ALL\n"));
  g_assert_nonnull (strstr (out, "CATEGORY-POWER: LOW\n"));
  g_assert_nonnull (strstr (out, "CATEGORY-MODE: MIXED\n"));
  g_assert_nonnull (strstr (out, "GRID-LOCATOR: JO60TD\n"));
  g_assert_nonnull (strstr (out, "CREATED-BY: log-for-linux"));
  g_assert_true (g_str_has_suffix (out, "END-OF-LOG:\n"));
  /* Optional empty fields stay out. */
  g_assert_null (strstr (out, "CLUB:"));
  g_assert_null (strstr (out, "EMAIL:"));

  /* Chronological: the SSB QSO (T) precedes the CW one (T+3600); the
   * main-log G3XYZ is absent. */
  const char *ssb = strstr (out, "14205 PH");
  const char *cw = strstr (out, " 7012 CW");
  g_assert_nonnull (ssb);
  g_assert_nonnull (cw);
  g_assert_true (ssb < cw);
  g_assert_null (strstr (out, "G3XYZ"));

  /* Full line shape (padded columns, sent serial zero-padded). */
  g_assert_nonnull (strstr (out,
      "QSO: 14205 PH 2025-08-01 1200 OK1BR         59  001    "
      "DL1AA         59  82\n"));
  g_assert_nonnull (strstr (out,
      "QSO:  7012 CW 2025-08-01 1300 OK1BR         599 002    "
      "S50A          579 63\n"));

  g_free (out);
  logfl_store_close (s);
}

static void
test_freq_mode_fallbacks (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  gint64 c = mk_contest (s);
  const gint64 T = 1754049600;

  /* No exact freq → generic band edge; FT8 → DG; missing RSTs default. */
  add_qso (s, c, "F5AAA", "15m", 0, "FT8", T,
           NULL, NULL, 0, "15", 0, "14");
  /* VHF: designator wins even with an exact frequency. */
  add_qso (s, c, "OK2BBB", "2m", 144.174, "SSB", T + 60,
           NULL, NULL, 0, "JN69", 0, "JO60");

  char *out = logfl_cabrillo_export (s, c, &OPTS, NULL, &err);
  g_assert_no_error (err);
  g_assert_nonnull (strstr (out,
      "QSO: 21000 DG 2025-08-01 1200 OK1BR         599 15     "
      "F5AAA         599 14\n"));
  g_assert_nonnull (strstr (out,
      "QSO:   144 PH 2025-08-01 1201 OK1BR         59  JN69   "
      "OK2BBB        59  JO60\n"));
  g_free (out);
  logfl_store_close (s);
}

static void
test_required_fields (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  gint64 c = mk_contest (s);

  LogflCabrilloOpts bad = { .contest = "EUHFC", .callsign = "" };
  g_assert_null (logfl_cabrillo_export (s, c, &bad, NULL, &err));
  g_assert_error (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID);
  g_clear_error (&err);

  bad.contest = NULL;
  bad.callsign = "OK1BR";
  g_assert_null (logfl_cabrillo_export (s, c, &bad, NULL, &err));
  g_assert_error (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID);
  g_clear_error (&err);

  logfl_store_close (s);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cabrillo/header-and-order", test_header_and_order);
  g_test_add_func ("/cabrillo/freq-mode-fallbacks",
                   test_freq_mode_fallbacks);
  g_test_add_func ("/cabrillo/required-fields", test_required_fields);
  return g_test_run ();
}
