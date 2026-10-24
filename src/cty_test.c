/* cty_test.c — gate for the cty.dat parser (src/engine/cty.c).
 *
 * Two layers: a synthetic mini-file exercising the format corners (exact
 * calls, zone/continent overrides, WAE-only '*' entities, portable calls),
 * then the real bundled data/cty.dat (path via argv[1] from meson) — the
 * snapshot must resolve the calls the app will actually meet.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "cty.h"

#include <glib/gstdio.h>
#include <string.h>

static const char *SAMPLE =
    "Czech Republic:           15:  28:  EU:   50.00:   -16.00:    -1.0:  OK:\n"
    "    OK,OL;\n"
    "United States:            05:  08:  NA:   37.60:    91.87:     5.0:  K:\n"
    "    AA,K,N,W,=4U1WB(5)[8],\n"
    "    =AJ3DI<12/34>~5~;\n"
    "European Turkey:          20:  39:  EU:   41.02:   -28.97:    -2.0:  *TA1:\n"
    "    TA1,=TC1X{AS};\n"
    /* Spans zones: the plain prefix gets the header's 05/09, VE2 knows its
     * ITU alone, VE7 both; VE2FK's [9] equals the default, yet is its own. */
    "Canada:                   05:  09:  NA:   44.35:    78.75:     5.0:  VE:\n"
    "    VA,VE,VE2[4],VE7(3)[2],=VE2FK[9];\n"
    "Version marker:           05:  08:  NA:    0.00:     0.00:     0.0:  VV:\n"
    "    =VER20260803;\n";

static char *sample_path;

static LogflCty *
load_sample (void)
{
  GError *err = NULL;
  LogflCty *c = logfl_cty_load (sample_path, &err);
  g_assert_no_error (err);
  g_assert_nonnull (c);
  return c;
}

static void
test_parse_and_basic (void)
{
  LogflCty *c = load_sample ();
  g_assert_cmpuint (logfl_cty_n_entities (c), ==, 5);
  g_assert_cmpstr (logfl_cty_version (c), ==, "20260803");

  LogflCtyInfo i;
  g_assert_true (logfl_cty_lookup (c, "ok1br", &i));
  g_assert_cmpstr (i.country, ==, "Czech Republic");
  g_assert_cmpstr (i.prefix, ==, "OK");
  g_assert_cmpstr (i.continent, ==, "EU");
  g_assert_cmpint (i.cq_zone, ==, 15);
  g_assert_cmpint (i.itu_zone, ==, 28);
  g_assert_false (i.waedc_only);

  g_assert_true (logfl_cty_lookup (c, "OL5X", &i));
  g_assert_cmpstr (i.country, ==, "Czech Republic");

  g_assert_false (logfl_cty_lookup (c, "ZZ9ZZZ", &i));
  g_assert_false (logfl_cty_lookup (c, "", &i));
  g_assert_false (logfl_cty_lookup (c, NULL, &i));
  logfl_cty_free (c);
}

static void
test_overrides (void)
{
  LogflCty *c = load_sample ();
  LogflCtyInfo i;

  /* Exact call with CQ/ITU overrides beats the K prefix defaults. */
  g_assert_true (logfl_cty_lookup (c, "4U1WB", &i));
  g_assert_cmpstr (i.country, ==, "United States");
  g_assert_cmpint (i.cq_zone, ==, 5);
  g_assert_cmpint (i.itu_zone, ==, 8);

  /* <lat/long> and ~tz~ decorations parse away cleanly. */
  g_assert_true (logfl_cty_lookup (c, "AJ3DI", &i));
  g_assert_cmpint (i.cq_zone, ==, 5);

  /* Continent override on an exact call. */
  g_assert_true (logfl_cty_lookup (c, "TC1X", &i));
  g_assert_cmpstr (i.continent, ==, "AS");

  /* '*' primary prefix: WAE-only entity, flag up, star stripped. */
  g_assert_true (logfl_cty_lookup (c, "TA1ABC", &i));
  g_assert_cmpstr (i.prefix, ==, "TA1");
  g_assert_true (i.waedc_only);
  logfl_cty_free (c);
}

static void
test_portable (void)
{
  LogflCty *c = load_sample ();
  LogflCtyInfo i;

  g_assert_true (logfl_cty_lookup (c, "OK1BR/P", &i));
  g_assert_cmpstr (i.country, ==, "Czech Republic");
  g_assert_true (logfl_cty_lookup (c, "W1AW/7", &i));
  g_assert_cmpstr (i.country, ==, "United States");
  g_assert_true (logfl_cty_lookup (c, "OK1BR/QRP", &i));
  g_assert_cmpstr (i.country, ==, "Czech Republic");

  /* Designator side decides for X/Y portables. */
  g_assert_true (logfl_cty_lookup (c, "OK/W1AW", &i));
  g_assert_cmpstr (i.country, ==, "Czech Republic");
  g_assert_true (logfl_cty_lookup (c, "W1AW/OK", &i));
  g_assert_cmpstr (i.country, ==, "Czech Republic");

  /* Maritime / aeronautical mobile: no country. */
  g_assert_false (logfl_cty_lookup (c, "OK1BR/MM", &i));
  g_assert_false (logfl_cty_lookup (c, "OK1BR/AM", &i));
  logfl_cty_free (c);
}

/* #28: a zone is "known" when the matched alias says so itself, or when
 * every alias of the entity shares it; an entity spanning zones hands its
 * plain prefixes a default that is only a guess. */
static void
test_zone_known (void)
{
  LogflCty *c = load_sample ();
  LogflCtyInfo i;

  g_assert_true (logfl_cty_lookup (c, "OK1BR", &i));
  g_assert_true (i.cq_zone_known);
  g_assert_true (i.itu_zone_known);

  /* The sample's US: 4U1WB's explicit (5)[8] equals the header, so the
   * entity spans nothing and a plain K is known. */
  g_assert_true (logfl_cty_lookup (c, "K1ABC", &i));
  g_assert_true (i.cq_zone_known);
  g_assert_true (i.itu_zone_known);

  /* Canada spans zones: the plain prefix guesses both … */
  g_assert_true (logfl_cty_lookup (c, "VE3ABC", &i));
  g_assert_cmpint (i.cq_zone, ==, 5);
  g_assert_cmpint (i.itu_zone, ==, 9);
  g_assert_false (i.cq_zone_known);
  g_assert_false (i.itu_zone_known);
  /* … an ITU-only override knows ITU and still guesses CQ … */
  g_assert_true (logfl_cty_lookup (c, "VE2ABC", &i));
  g_assert_cmpint (i.itu_zone, ==, 4);
  g_assert_true (i.itu_zone_known);
  g_assert_false (i.cq_zone_known);
  /* … both overrides know both … */
  g_assert_true (logfl_cty_lookup (c, "VE7ABC", &i));
  g_assert_cmpint (i.cq_zone, ==, 3);
  g_assert_cmpint (i.itu_zone, ==, 2);
  g_assert_true (i.cq_zone_known);
  g_assert_true (i.itu_zone_known);
  /* … and an exact call's override equal to the default is still its own. */
  g_assert_true (logfl_cty_lookup (c, "VE2FK", &i));
  g_assert_cmpint (i.itu_zone, ==, 9);
  g_assert_true (i.itu_zone_known);
  g_assert_false (i.cq_zone_known);
  logfl_cty_free (c);
}

/* --- the real bundled snapshot ------------------------------------------ */

static const char *real_path;

static void
test_real_file (void)
{
  GError *err = NULL;
  LogflCty *c = logfl_cty_load (real_path, &err);
  g_assert_no_error (err);
  g_assert_nonnull (c);

  /* A plausible snapshot: hundreds of entities, dated version marker. */
  g_assert_cmpuint (logfl_cty_n_entities (c), >, 300);
  g_assert_nonnull (logfl_cty_version (c));
  g_assert_cmpuint (strlen (logfl_cty_version (c)), ==, 8);

  LogflCtyInfo i;
  g_assert_true (logfl_cty_lookup (c, "OK1BR", &i));
  g_assert_cmpstr (i.country, ==, "Czech Republic");
  g_assert_cmpstr (i.continent, ==, "EU");
  g_assert_true (logfl_cty_lookup (c, "OL9XYZ", &i));
  g_assert_cmpstr (i.country, ==, "Czech Republic");

  g_assert_true (logfl_cty_lookup (c, "OH1VR", &i));
  g_assert_cmpstr (i.country, ==, "Finland");
  g_assert_cmpstr (i.continent, ==, "EU");

  g_assert_true (logfl_cty_lookup (c, "K1ABC", &i));
  g_assert_cmpstr (i.continent, ==, "NA");
  g_assert_true (logfl_cty_lookup (c, "PY2XYZ", &i));
  g_assert_cmpstr (i.country, ==, "Brazil");
  g_assert_cmpstr (i.continent, ==, "SA");
  g_assert_true (logfl_cty_lookup (c, "JA1ABC", &i));
  g_assert_cmpstr (i.continent, ==, "AS");
  g_assert_true (logfl_cty_lookup (c, "VK2DEF", &i));
  g_assert_cmpstr (i.continent, ==, "OC");

  /* Longest prefix beats shorter: 3B8 vs 3B6 family. */
  g_assert_true (logfl_cty_lookup (c, "3B8CF", &i));
  g_assert_cmpstr (i.country, ==, "Mauritius");

  /* #28 zone certainty over the snapshot: Germany is one zone; the US
   * spans CQ 3/4/5 through exact calls only, so a plain K is a guess while
   * AA6KJ's own (4)[7] is not; Canada's VE2FK carries [9] alone. */
  g_assert_true (logfl_cty_lookup (c, "DL1ABC", &i));
  g_assert_cmpint (i.cq_zone, ==, 14);
  g_assert_true (i.cq_zone_known);
  g_assert_true (i.itu_zone_known);
  g_assert_true (logfl_cty_lookup (c, "K7ABC", &i));
  g_assert_false (i.cq_zone_known);
  g_assert_false (i.itu_zone_known);
  g_assert_true (logfl_cty_lookup (c, "AA6KJ", &i));
  g_assert_cmpint (i.cq_zone, ==, 4);
  g_assert_cmpint (i.itu_zone, ==, 7);
  g_assert_true (i.cq_zone_known);
  g_assert_true (i.itu_zone_known);
  g_assert_true (logfl_cty_lookup (c, "VE2FK", &i));
  g_assert_cmpint (i.itu_zone, ==, 9);
  g_assert_true (i.itu_zone_known);
  g_assert_false (i.cq_zone_known);
  logfl_cty_free (c);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, >=, 2);   /* meson passes data/cty.dat */
  real_path = argv[1];

  GError *err = NULL;
  sample_path = g_build_filename (g_get_tmp_dir (), "logfl-cty-sample.dat",
                                  NULL);
  g_assert_true (g_file_set_contents (sample_path, SAMPLE, -1, &err));
  g_assert_no_error (err);

  g_test_add_func ("/cty/parse-basic", test_parse_and_basic);
  g_test_add_func ("/cty/overrides", test_overrides);
  g_test_add_func ("/cty/zone-known", test_zone_known);
  g_test_add_func ("/cty/portable", test_portable);
  g_test_add_func ("/cty/real-file", test_real_file);

  int rc = g_test_run ();
  g_unlink (sample_path);
  g_free (sample_path);
  return rc;
}
