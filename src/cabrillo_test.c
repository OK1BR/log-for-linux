/* cabrillo_test.c — Cabrillo gate (log-cabrillo-test): header tags, QSO
 * line shape (kHz / generic band freq / VHF designator, mode mapping,
 * RST defaults, serial + text exchanges), chronological order and the
 * required-field errors. Headless.
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

/* The 2026-08-23 regression: a CW-only log must never offer RTTY just
 * because the previous contest was RTTY. Categories come from the QSOs. */
static void
test_categories_from_log (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  gint64 c = mk_contest (s);
  const gint64 T = 1754049600;
  char *mode = NULL, *band = NULL;

  /* Empty contest: the log cannot say, both stay NULL. */
  g_assert_true (logfl_cabrillo_categories_from_log (s, c, &mode, &band,
                                                     &err));
  g_assert_no_error (err);
  g_assert_null (mode);
  g_assert_null (band);

  /* Single band, single mode. */
  add_qso (s, c, "HA6NL", "20m", 14.005, "CW", T, "599", "599",
           1, NULL, 23, NULL);
  add_qso (s, c, "YR8E", "20m", 14.025, "CW", T + 60, "599", "599",
           2, NULL, 0, "BT");
  g_assert_true (logfl_cabrillo_categories_from_log (s, c, &mode, &band,
                                                     &err));
  g_assert_cmpstr (mode, ==, "CW");
  g_assert_cmpstr (band, ==, "20M");
  g_clear_pointer (&mode, g_free);
  g_clear_pointer (&band, g_free);

  /* A QSO in the main log must not move the contest's categories. */
  add_qso (s, 0, "G3XYZ", "40m", 7.02, "SSB", T + 120, NULL, NULL,
           0, NULL, 0, NULL);
  g_assert_true (logfl_cabrillo_categories_from_log (s, c, &mode, &band,
                                                     &err));
  g_assert_cmpstr (mode, ==, "CW");
  g_assert_cmpstr (band, ==, "20M");
  g_clear_pointer (&mode, g_free);
  g_clear_pointer (&band, g_free);

  /* Second band and a phone QSO: ALL / MIXED. */
  add_qso (s, c, "DL1AA", "40m", 7.012, "SSB", T + 180, "59", "59",
           3, NULL, 82, NULL);
  g_assert_true (logfl_cabrillo_categories_from_log (s, c, &mode, &band,
                                                     &err));
  g_assert_cmpstr (mode, ==, "MIXED");
  g_assert_cmpstr (band, ==, "ALL");
  g_clear_pointer (&mode, g_free);
  g_clear_pointer (&band, g_free);
  logfl_store_close (s);

  /* Mode families, not raw ADIF modes: FT8 and PSK31 are one DIGI entry. */
  s = mem_store ();
  c = mk_contest (s);
  add_qso (s, c, "OK2ABC", "20m", 14.074, "FT8", T, "-10", "-12",
           0, NULL, 0, NULL);
  add_qso (s, c, "OK2DEF", "20m", 14.070, "PSK31", T + 60, "599", "599",
           0, NULL, 0, NULL);
  g_assert_true (logfl_cabrillo_categories_from_log (s, c, &mode, &band,
                                                     &err));
  g_assert_cmpstr (mode, ==, "DIGI");
  g_assert_cmpstr (band, ==, "20M");
  g_clear_pointer (&mode, g_free);
  g_clear_pointer (&band, g_free);

  /* An unmapped band cannot be named — band goes back to NULL, mode
   * (which the band cannot spoil) still answers. */
  add_qso (s, c, "OK2GHI", "13cm", 2320.1, "CW", T + 120, "599", "599",
           0, NULL, 0, NULL);
  g_assert_true (logfl_cabrillo_categories_from_log (s, c, &mode, &band,
                                                     &err));
  g_assert_cmpstr (mode, ==, "MIXED");
  g_assert_null (band);
  g_free (mode);

  logfl_store_close (s);
}

/* The 2026-09-20 regression: a 20 m log from outside Scandinavia went out
 * as CATEGORY-BAND: 20M. The log's band is a wish; SAC's list decides. */
static void
test_pick_and_new_tags (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  gint64 c = mk_contest (s);
  const gint64 T = 1758326400;         /* 2026-09-20 00:00:00 UTC */
  add_qso (s, c, "SM5AAA", "20m", 14.021, "CW", T, "599", "599",
           1, NULL, 12, NULL);
  add_qso (s, c, "OH2BBB", "20m", 14.033, "CW", T + 60, "599", "599",
           2, NULL, 45, NULL);

  char *mode = NULL, *band = NULL;
  g_assert_true (logfl_cabrillo_categories_from_log (s, c, &mode, &band,
                                                     &err));
  g_assert_cmpstr (band, ==, "20M");

  guint n = 0;
  const LogflContestPreset *p = logfl_contest_presets (&n);
  const LogflContestPreset *sac_p = NULL;
  for (guint i = 0; i < n; i++)
    if (g_str_equal (p[i].name, "SAC"))
      sac_p = &p[i];
  g_assert_nonnull (sac_p);
  LogflExchDef *sac = logfl_exch_def_parse (sac_p->exch_def, &err);
  g_assert_no_error (err);

  /* From OK: 20M is not on the list -> the default, ALL. */
  const char *const *outside =
      logfl_exch_def_cab_values (sac, LOGFL_CAB_BAND, NULL);
  g_assert_cmpstr (outside[logfl_cabrillo_pick (outside, band)], ==, "ALL");
  /* From Sweden the same log is a legal single-band entry. */
  LogflCtyInfo sm = { .country = "Sweden", .prefix = "SM",
                      .continent = "EU" };
  const char *const *inside =
      logfl_exch_def_cab_values (sac, LOGFL_CAB_BAND, &sm);
  g_assert_cmpstr (inside[logfl_cabrillo_pick (inside, band)], ==, "20M");
  /* Nothing wanted, or no list at all: the first entry. */
  g_assert_cmpuint (logfl_cabrillo_pick (outside, NULL), ==, 0);
  g_assert_cmpuint (logfl_cabrillo_pick (outside, ""), ==, 0);
  g_assert_cmpuint (logfl_cabrillo_pick (NULL, "20M"), ==, 0);
  g_free (mode);
  g_free (band);
  logfl_exch_def_free (sac);

  /* Station and overlay reach the header; left empty they stay out. */
  LogflCabrilloOpts o = OPTS;
  o.cat_band = "LOW-BAND";
  o.cat_station = "FIXED";
  o.cat_overlay = "WIRE-ONLY";
  char *out = logfl_cabrillo_export (s, c, &o, NULL, &err);
  g_assert_no_error (err);
  g_assert_nonnull (strstr (out, "CATEGORY-BAND: LOW-BAND\n"));
  g_assert_nonnull (strstr (out, "CATEGORY-STATION: FIXED\n"));
  g_assert_nonnull (strstr (out, "CATEGORY-OVERLAY: WIRE-ONLY\n"));
  g_free (out);
  out = logfl_cabrillo_export (s, c, &OPTS, NULL, &err);
  g_assert_no_error (err);
  g_assert_null (strstr (out, "CATEGORY-STATION"));
  g_assert_null (strstr (out, "CATEGORY-OVERLAY"));
  g_assert_null (strstr (out, "CATEGORY-ASSISTED"));
  g_free (out);

  logfl_store_close (s);
}

/* CQ WW RTTY (cqwwrtty.com/cabrillo.htm): the QSO line carries zone + QTH
 * on both sides, "DX" where a station outside W/VE sends none — from the
 * contest's own definition (cab_placeholder), our sent side included. */
static void
test_placeholders (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  guint n = 0;
  const LogflContestPreset *p = logfl_contest_presets (&n);
  const LogflContestPreset *rtty = NULL;
  for (guint i = 0; i < n; i++)
    if (g_str_equal (p[i].name, "CQ WW RTTY"))
      rtty = &p[i];
  g_assert_nonnull (rtty);
  LogflContest *c = logfl_contest_new ();
  c->name = g_strdup ("CQ WW RTTY 2026");
  c->adif_id = g_strdup (rtty->adif_id);
  c->exch_def = g_strdup (rtty->exch_def);
  c->my_exch = g_strdup ("15");
  g_assert_true (logfl_store_contest_add (s, c, &err));
  gint64 id = c->id;
  logfl_contest_free (c);

  const gint64 T = 1790380800;         /* 2026-09-26 00:00:00 UTC */
  add_qso (s, id, "K1AB", "20m", 14.085, "RTTY", T, "599", "599",
           0, "15", 0, "05 MA");
  add_qso (s, id, "DL1AB", "20m", 14.086, "RTTY", T + 60, "599", "599",
           0, "15", 0, "14");
  /* Nothing received at all: the zone slot has no placeholder, so the
   * fill never starts and the gap stays visible as "-". */
  add_qso (s, id, "G3XYZ", "20m", 14.087, "RTTY", T + 120, "599", "599",
           0, "15", 0, NULL);

  LogflCabrilloOpts o = OPTS;
  o.contest = "CQ-WW-RTTY";
  o.cat_mode = "RTTY";
  char *out = logfl_cabrillo_export (s, id, &o, &n, &err);
  g_assert_no_error (err);
  g_assert_cmpuint (n, ==, 3);
  char *sent = g_strdup_printf ("%-13s 599 %-6s %-13s 599 05 MA\n",
                                "OK1BR", "15 DX", "K1AB");
  g_assert_nonnull (strstr (out, sent));
  char *dl = g_strdup_printf ("%-13s 599 14 DX\n", "DL1AB");
  g_assert_nonnull (strstr (out, dl));
  char *g = g_strdup_printf ("%-13s 599 -\n", "G3XYZ");
  g_assert_nonnull (strstr (out, g));
  g_free (g);
  g_free (dl);
  g_free (sent);
  g_free (out);
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
  g_test_add_func ("/cabrillo/categories-from-log",
                   test_categories_from_log);
  g_test_add_func ("/cabrillo/pick-and-new-tags", test_pick_and_new_tags);
  g_test_add_func ("/cabrillo/placeholders", test_placeholders);
  return g_test_run ();
}
