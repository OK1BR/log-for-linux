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

  /* Serialize → parse → identical shape (incl. the validity rule). */
  def->counts = LOGFL_COUNTS_EU_DX;
  def->zero_own_country = TRUE;
  char *text = logfl_exch_def_serialize (def);
  LogflExchDef *back = logfl_exch_def_parse (text, &err);
  g_assert_no_error (err);
  g_assert_true (back->tx_serial);
  g_assert_cmpint (back->counts, ==, LOGFL_COUNTS_EU_DX);
  g_assert_true (back->zero_own_country);
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

  /* An unknown counts rule must fail loud, not degrade to "all". */
  g_assert_null (logfl_exch_def_parse (
      "[exchange]\nfields=a;\ncounts=marsonly\n[field:a]\n", &err));
  g_assert_error (err, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE);
  g_clear_error (&err);
}

/* --- QSO validity under the contest rule -------------------------------- */

static LogflCtyInfo
cty_of (const char *country, const char *prefix, const char *cont)
{
  LogflCtyInfo i = { 0 };
  i.country = country;
  i.prefix = prefix;
  i.continent[0] = cont[0];
  i.continent[1] = cont[1];
  return i;
}

static const LogflContestPreset *
preset_named (const char *name)
{
  guint n = 0;
  const LogflContestPreset *p = logfl_contest_presets (&n);
  for (guint i = 0; i < n; i++)
    if (g_str_equal (p[i].name, name))
      return &p[i];
  g_assert_not_reached ();
}

static void
test_qso_validity (void)
{
  GError *err = NULL;
  LogflCtyInfo ok = cty_of ("Czech Republic", "OK", "EU");
  LogflCtyInfo oh = cty_of ("Finland", "OH", "EU");
  LogflCtyInfo k  = cty_of ("United States", "K", "NA");
  LogflCtyInfo py = cty_of ("Brazil", "PY", "SA");

  /* WAE: only QSOs crossing the EU boundary count. */
  LogflExchDef *wae =
      logfl_exch_def_parse (preset_named ("WAE DX")->exch_def, &err);
  g_assert_no_error (err);
  g_assert_cmpint (wae->counts, ==, LOGFL_COUNTS_EU_DX);
  g_assert_cmpint (logfl_contest_qso_validity (wae, &ok, &k), ==,
                   LOGFL_QSO_VALID);
  g_assert_cmpint (logfl_contest_qso_validity (wae, &ok, &oh), ==,
                   LOGFL_QSO_NOT_VALID);
  g_assert_cmpint (logfl_contest_qso_validity (wae, &k, &py), ==,
                   LOGFL_QSO_NOT_VALID);
  g_assert_cmpint (logfl_contest_qso_validity (wae, &k, &ok), ==,
                   LOGFL_QSO_VALID);
  /* Unresolved side: benefit of the doubt. */
  g_assert_cmpint (logfl_contest_qso_validity (wae, &ok, NULL), ==,
                   LOGFL_QSO_VALID);
  g_assert_cmpint (logfl_contest_qso_validity (wae, NULL, &oh), ==,
                   LOGFL_QSO_VALID);
  logfl_exch_def_free (wae);

  /* EUHFC: Europeans only — judged from their side alone. */
  LogflExchDef *euhfc =
      logfl_exch_def_parse (preset_named ("EUHFC")->exch_def, &err);
  g_assert_no_error (err);
  g_assert_cmpint (euhfc->counts, ==, LOGFL_COUNTS_EU_ONLY);
  g_assert_cmpint (logfl_contest_qso_validity (euhfc, &ok, &oh), ==,
                   LOGFL_QSO_VALID);
  g_assert_cmpint (logfl_contest_qso_validity (euhfc, &ok, &k), ==,
                   LOGFL_QSO_NOT_VALID);
  g_assert_cmpint (logfl_contest_qso_validity (euhfc, NULL, &py), ==,
                   LOGFL_QSO_NOT_VALID);
  logfl_exch_def_free (euhfc);

  /* CQ WW: everyone counts, own country scores zero. */
  LogflExchDef *cqww =
      logfl_exch_def_parse (preset_named ("CQ WW")->exch_def, &err);
  g_assert_no_error (err);
  g_assert_true (cqww->zero_own_country);
  LogflCtyInfo ol = cty_of ("Czech Republic", "OK", "EU");
  g_assert_cmpint (logfl_contest_qso_validity (cqww, &ok, &ol), ==,
                   LOGFL_QSO_ZERO_POINTS);
  g_assert_cmpint (logfl_contest_qso_validity (cqww, &ok, &oh), ==,
                   LOGFL_QSO_VALID);
  logfl_exch_def_free (cqww);

  /* Presets without a rule accept anything. */
  LogflExchDef *wpx =
      logfl_exch_def_parse (preset_named ("CQ WPX")->exch_def, &err);
  g_assert_no_error (err);
  g_assert_cmpint (wpx->counts, ==, LOGFL_COUNTS_ALL);
  g_assert_cmpint (logfl_contest_qso_validity (wpx, &ok, &ol), ==,
                   LOGFL_QSO_VALID);
  logfl_exch_def_free (wpx);
}

/* Contests stored before validity rules existed pick theirs up from the
 * ADIF id; explicit defs (even counts=all) stay untouched. */
static void
test_backfill_validity (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  static const char *OLD_DEF =
      "[exchange]\ntx_serial=true\nfields=nr;\n"
      "[field:nr]\nlabel=Nr\ntype=serial\nrequired=true\n";

  LogflContest *wae = logfl_contest_new ();
  wae->name = g_strdup ("WAE DX 2026");
  wae->adif_id = g_strdup ("DARC-WAEDC-CW");
  wae->exch_def = g_strdup (OLD_DEF);
  g_assert_true (logfl_store_contest_add (s, wae, &err));

  LogflContest *euhfc = logfl_contest_new ();
  euhfc->name = g_strdup ("EUHFC 2026");
  euhfc->adif_id = g_strdup ("EU-HF");
  euhfc->exch_def = g_strdup (OLD_DEF);
  g_assert_true (logfl_store_contest_add (s, euhfc, &err));

  /* Operator's own choice: explicit counts=all must survive. */
  LogflContest *own = logfl_contest_new ();
  own->name = g_strdup ("WAE for fun");
  own->adif_id = g_strdup ("DARC-WAEDC-SSB");
  own->exch_def = g_strdup (
      "[exchange]\ntx_serial=true\nfields=nr;\ncounts=all\n"
      "[field:nr]\nlabel=Nr\ntype=serial\nrequired=true\n");
  g_assert_true (logfl_store_contest_add (s, own, &err));

  /* No ADIF id → nothing to infer from. */
  LogflContest *cust = logfl_contest_new ();
  cust->name = g_strdup ("Custom thing");
  cust->exch_def = g_strdup (OLD_DEF);
  g_assert_true (logfl_store_contest_add (s, cust, &err));

  /* All three preset-mapped contests get repaired: WAE and EUHFC gain
   * validity + scoring, the counts=all one keeps its validity edit but
   * still gains the scoring keys (independent knobs, LOG-3). */
  g_assert_cmpuint (logfl_contest_backfill_validity (s, &err), ==, 3);
  g_assert_no_error (err);

  LogflContest *back = logfl_store_contest_get (s, wae->id, &err);
  LogflExchDef *def = logfl_exch_def_parse (back->exch_def, &err);
  g_assert_no_error (err);
  g_assert_cmpint (def->counts, ==, LOGFL_COUNTS_EU_DX);
  g_assert_true (def->tx_serial);          /* the rest of the def survived */
  g_assert_cmpuint (def->fields->len, ==, 1);
  g_assert_nonnull (def->points);          /* LOG-3 scoring rode along */
  g_assert_cmpuint (def->mult, ==, LOGFL_MULT_COUNTRY_AREAS);
  g_assert_nonnull (def->mult_weight);
  logfl_exch_def_free (def);
  logfl_contest_free (back);

  back = logfl_store_contest_get (s, euhfc->id, &err);
  def = logfl_exch_def_parse (back->exch_def, &err);
  g_assert_cmpint (def->counts, ==, LOGFL_COUNTS_EU_ONLY);
  g_assert_cmpuint (def->mult, ==, LOGFL_MULT_EXCH);
  logfl_exch_def_free (def);
  logfl_contest_free (back);

  back = logfl_store_contest_get (s, own->id, &err);
  def = logfl_exch_def_parse (back->exch_def, &err);
  g_assert_cmpint (def->counts, ==, LOGFL_COUNTS_ALL);
  g_assert_nonnull (def->points);
  logfl_exch_def_free (def);
  logfl_contest_free (back);

  /* Second run: nothing left to do. */
  g_assert_cmpuint (logfl_contest_backfill_validity (s, &err), ==, 0);
  g_assert_no_error (err);

  logfl_contest_free (wae);
  logfl_contest_free (euhfc);
  logfl_contest_free (own);
  logfl_contest_free (cust);
  logfl_store_close (s);
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

  /* WAE/SARTG are serial contests; CVA's continent/state is TEXT (a PY
   * state like "SP" must never be reinterpreted as a number). */
  gboolean wae = FALSE, cva = FALSE, sartg = FALSE, yodx = FALSE;
  for (guint i = 0; i < n; i++)
    {
      LogflExchFieldType want;
      gboolean want_serial;
      if (g_strcmp0 (p[i].name, "WAE DX") == 0)
        {
          wae = TRUE;
          g_assert_cmpstr (p[i].adif_id, ==, "DARC-WAEDC-CW");
          want = LOGFL_EXCH_SERIAL;
          want_serial = TRUE;
        }
      else if (g_strcmp0 (p[i].name, "CVA DX") == 0)
        {
          cva = TRUE;
          g_assert_cmpstr (p[i].adif_id, ==, "CVA-DX-CW");
          want = LOGFL_EXCH_TEXT;
          want_serial = FALSE;
        }
      else if (g_strcmp0 (p[i].name, "YO DX") == 0)
        {
          /* Mixed exchange: a YO county is text, a DX serial is digits. */
          yodx = TRUE;
          g_assert_cmpstr (p[i].adif_id, ==, "YOHFDX");
          want = LOGFL_EXCH_AUTO;
          want_serial = TRUE;
        }
      else if (g_strcmp0 (p[i].name, "SARTG WW RTTY") == 0)
        {
          sartg = TRUE;
          g_assert_cmpstr (p[i].adif_id, ==, "SARTG-RTTY");
          want = LOGFL_EXCH_SERIAL;
          want_serial = TRUE;
        }
      else
        continue;
      def = logfl_exch_def_parse (p[i].exch_def, &err);
      g_assert_no_error (err);
      f = def->fields->pdata[0];
      g_assert_cmpint (f->type, ==, want);
      g_assert_cmpint (def->tx_serial, ==, want_serial);
      logfl_exch_def_free (def);
    }
  g_assert_true (wae && cva && sartg && yodx);

  /* YO DX: everyone works everyone and no QSO scores zero from an OK seat
   * (rules §6.1) — the preset must carry no validity rule at all. */
  def = logfl_exch_def_parse (preset_named ("YO DX")->exch_def, &err);
  g_assert_no_error (err);
  g_assert_cmpint (def->counts, ==, LOGFL_COUNTS_ALL);
  g_assert_false (def->zero_own_country);
  g_assert_true (((LogflExchField *) def->fields->pdata[0])->required);
  logfl_exch_def_free (def);
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

  /* Worked-B4 follows the scope: whole log, one contest, or main only.
   * OK1ABC sits 2× in contest A, 0× in B, 1× in the main log. */
  LogflWorkedB4 wb;
  g_assert_true (logfl_store_worked_b4 (s, LOGFL_QUERY_CONTEST_ALL, "OK1ABC",
                                        "40m", "CW", &wb, &err));
  g_assert_cmpuint (wb.n_total, ==, 3);
  g_assert_true (logfl_store_worked_b4 (s, a->id, "OK1ABC", "40m", "CW",
                                        &wb, &err));
  g_assert_cmpuint (wb.n_total, ==, 2);
  g_assert_cmpuint (wb.n_band, ==, 1);
  g_assert_cmpuint (wb.n_band_mode, ==, 1);
  /* Contest B never worked him — B4 must not leak in from elsewhere. */
  g_assert_true (logfl_store_worked_b4 (s, b->id, "OK1ABC", "40m", "CW",
                                        &wb, &err));
  g_assert_cmpuint (wb.n_total, ==, 0);
  g_assert_cmpint (wb.last_ts, ==, 0);
  g_assert_true (logfl_store_worked_b4 (s, LOGFL_QUERY_CONTEST_NONE, "OK1ABC",
                                        "40m", "CW", &wb, &err));
  g_assert_cmpuint (wb.n_total, ==, 1);

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

/* The window exports what it shows — a contest-scoped query must yield only
 * that contest's QSOs and CONTEST_NONE only the main log. Guards the engine
 * side of the 2026-08-09 bug where the UI exported the whole store. */
static void
test_adif_export_scoped (void)
{
  GError *err = NULL;
  LogflStore *s = mem_store ();
  guint np = 0;
  const LogflContestPreset *p = logfl_contest_presets (&np);
  LogflContest *c = mk_contest (s, "CQ WPX CW 2026", "CQ-WPX-CW",
                                p[1].exch_def);

  LogflQso *q = mk_qso ("OK1ABC", "40m", "CW", 1700000000);
  q->contest_ref = c->id;
  g_assert_true (logfl_store_add (s, q, &err));
  logfl_qso_free (q);
  q = mk_qso ("G4XYZ", "20m", "SSB", 1700000100);
  g_assert_true (logfl_store_add (s, q, &err));
  logfl_qso_free (q);

  guint n = 0;
  LogflStoreQuery in_contest = { .contest = c->id };
  char *data = logfl_adif_export_data (s, &in_contest, &n, &err);
  g_assert_no_error (err);
  g_assert_cmpuint (n, ==, 1);
  g_assert_nonnull (strstr (data, "<CALL:6>OK1ABC"));
  g_assert_null (strstr (data, "G4XYZ"));
  g_free (data);

  LogflStoreQuery main_log = { .contest = LOGFL_QUERY_CONTEST_NONE };
  data = logfl_adif_export_data (s, &main_log, &n, &err);
  g_assert_no_error (err);
  g_assert_cmpuint (n, ==, 1);
  g_assert_nonnull (strstr (data, "<CALL:5>G4XYZ"));
  g_assert_null (strstr (data, "OK1ABC"));
  g_free (data);

  logfl_contest_free (c);
  logfl_store_close (s);
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

/* --- LOG-3 scoring ------------------------------------------------------ */

static const char *cty_path;     /* data/cty.dat, argv[1] from meson */

static LogflCty *
load_cty (void)
{
  GError *err = NULL;
  LogflCty *c = logfl_cty_load (cty_path, &err);
  g_assert_no_error (err);
  g_assert_nonnull (c);
  return c;
}

/* A scored QSO; the array mirrors logfl_store_list order (newest FIRST),
 * so tests must prepend chronologically — or insert at index 0. */
static void
add_sqso (GPtrArray *a, gint64 id, const char *call, const char *band,
          const char *mode, const char *srx_string)
{
  LogflQso *q = mk_qso (call, band, mode, 1000 + id);
  q->id = id;
  q->srx_string = g_strdup (srx_string);
  g_ptr_array_insert (a, 0, q);
}

static const LogflQsoScore *
score_of (GHashTable *scores, gint64 id)
{
  const LogflQsoScore *sc = g_hash_table_lookup (scores, &id);
  g_assert_nonnull (sc);
  return sc;
}

static void
test_score_roundtrip (void)
{
  GError *err = NULL;
  LogflExchDef *def = logfl_exch_def_parse (
      "[exchange]\ntx_serial=true\nfields=nr;\n"
      "points=country:YO=8;own-country=1;same-cont=2;other-cont=3/6;\n"
      "mult=exch:YO+country\nmult_scope=contest\n"
      "mult_weight=80m:4;40m:3;\n"
      "[field:nr]\nlabel=Nr\ntype=serial\n", &err);
  g_assert_no_error (err);
  g_assert_nonnull (def->points);
  g_assert_cmpuint (def->points->len, ==, 4);
  LogflPtsTerm *t = &g_array_index (def->points, LogflPtsTerm, 0);
  g_assert_cmpint (t->kind, ==, LOGFL_PTS_COUNTRY);
  g_assert_cmpstr (t->arg, ==, "YO");
  g_assert_cmpint (t->points, ==, 8);
  g_assert_cmpint (t->points_low, ==, 8);
  t = &g_array_index (def->points, LogflPtsTerm, 3);
  g_assert_cmpint (t->points, ==, 3);
  g_assert_cmpint (t->points_low, ==, 6);
  g_assert_cmpuint (def->mult, ==, LOGFL_MULT_EXCH | LOGFL_MULT_COUNTRY);
  g_assert_cmpstr (def->mult_exch_from, ==, "YO");
  g_assert_true (def->mult_per_contest);
  g_assert_nonnull (def->mult_weight);
  g_assert_cmpint (
      GPOINTER_TO_INT (g_hash_table_lookup (def->mult_weight, "40m")),
      ==, 3);

  /* Serialize → parse keeps every scoring detail. */
  char *text = logfl_exch_def_serialize (def);
  LogflExchDef *back = logfl_exch_def_parse (text, &err);
  g_assert_no_error (err);
  g_assert_cmpuint (back->points->len, ==, 4);
  t = &g_array_index (back->points, LogflPtsTerm, 3);
  g_assert_cmpint (t->points_low, ==, 6);
  g_assert_cmpuint (back->mult, ==, def->mult);
  g_assert_cmpstr (back->mult_exch_from, ==, "YO");
  g_assert_true (back->mult_per_contest);
  g_assert_cmpint (
      GPOINTER_TO_INT (g_hash_table_lookup (back->mult_weight, "80m")),
      ==, 4);
  g_free (text);
  logfl_exch_def_free (back);
  logfl_exch_def_free (def);
}

static void
test_score_errors (void)
{
  /* An unknown rule must fail the parse loudly, like counts= does —
   * silently scoring wrong is the one forbidden failure mode. */
  static const char *const bad[] = {
    "[exchange]\nfields=nr;\npoints=frobnicate=3;\n",
    "[exchange]\nfields=nr;\npoints=own-country;\n",
    "[exchange]\nfields=nr;\nmult=nonsense\n",
    "[exchange]\nfields=nr;\nmult=exch\nmult_scope=weekly\n",
    "[exchange]\nfields=nr;\nmult=exch\nmult_weight=80m:x;\n",
  };
  for (gsize i = 0; i < G_N_ELEMENTS (bad); i++)
    {
      GError *err = NULL;
      g_assert_null (logfl_exch_def_parse (bad[i], &err));
      g_assert_error (err, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE);
      g_clear_error (&err);
    }
}

static void
test_wpx_prefix (void)
{
  static const struct { const char *call, *pfx; } cases[] = {
    { "OK1BR", "OK1" },     { "N8ABC", "N8" },
    { "LY1000XX", "LY1000" }, { "7M4XYZ", "7M4" },
    { "XEFTJW", "XE0" },    { "PA/N8BJQ", "PA0" },
    { "K5DJ/1", "K1" },     { "OK1BR/P", "OK1" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      char *p = logfl_wpx_prefix (cases[i].call);
      g_assert_cmpstr (p, ==, cases[i].pfx);
      g_free (p);
    }
}

static void
test_score_yodx (void)
{
  GError *err = NULL;
  guint n = 0;
  const LogflContestPreset *p = logfl_contest_presets (&n);
  const char *yodx = NULL;
  for (guint i = 0; i < n; i++)
    if (g_strcmp0 (p[i].adif_id, "YOHFDX") == 0)
      yodx = p[i].exch_def;
  LogflExchDef *def = logfl_exch_def_parse (yodx, &err);
  g_assert_no_error (err);

  LogflCty *cty = load_cty ();
  GPtrArray *qsos = g_ptr_array_new_with_free_func (
      (GDestroyNotify) logfl_qso_free);
  add_sqso (qsos, 1, "YO3GCL", "20m", "CW", "BU");   /* 8 pts, YO + BU  */
  add_sqso (qsos, 2, "DL1AB", "20m", "CW", NULL);    /* 2 pts, DL       */
  add_sqso (qsos, 3, "K1AB", "20m", "CW", NULL);     /* 4 pts, K        */
  add_sqso (qsos, 4, "OK2XYZ", "20m", "CW", NULL);   /* 1 pt,  OK       */
  add_sqso (qsos, 5, "YO3GCL", "20m", "CW", "BU");   /* dupe: 0, none   */
  add_sqso (qsos, 6, "YR8D", "40m", "CW", "SV");     /* 8 pts, YO + SV
                                                        again — per band */
  add_sqso (qsos, 7, "CT7XX", "20m", "CW", NULL);    /* 2 pts, CT country */
  add_sqso (qsos, 8, "YO4NF", "20m", "CW", "CT");    /* 8 pts, CT county —
                                                        must NOT be
                                                        swallowed by the
                                                        Portugal prefix */
  LogflContestTotals tot;
  GHashTable *scores =
    logfl_contest_score (def, cty, "OK1BR", qsos, &tot);
  g_assert_nonnull (scores);
  g_assert_true (tot.have_points);
  g_assert_true (tot.have_mult);
  g_assert_cmpint (tot.points, ==, 33);
  g_assert_cmpint (tot.mults, ==, 9);
  g_assert_cmpint (tot.total, ==, 297);
  g_assert_cmpstr (score_of (scores, 8)->mult, ==, "CT");

  g_assert_cmpint (score_of (scores, 1)->points, ==, 8);
  g_assert_cmpstr (score_of (scores, 1)->mult, ==, "YO BU");
  g_assert_cmpint (score_of (scores, 4)->points, ==, 1);
  g_assert_cmpstr (score_of (scores, 4)->mult, ==, "OK");
  g_assert_cmpint (score_of (scores, 5)->points, ==, 0);
  g_assert_null (score_of (scores, 5)->mult);
  g_assert_cmpstr (score_of (scores, 6)->mult, ==, "YO SV");

  g_hash_table_unref (scores);
  g_ptr_array_unref (qsos);
  logfl_cty_free (cty);
  logfl_exch_def_free (def);
}

static void
test_score_cqww (void)
{
  GError *err = NULL;
  LogflExchDef *def = logfl_exch_def_parse (
      "[exchange]\nfields=zone;\nzero_own_country=true\n"
      "points=own-country=0;same-cont=1;other-cont=3;\n"
      "mult=cqzone+country\n"
      "[field:zone]\nlabel=Zone\ntype=number\nadif_num=CQZ\n", &err);
  g_assert_no_error (err);

  LogflCty *cty = load_cty ();
  GPtrArray *qsos = g_ptr_array_new_with_free_func (
      (GDestroyNotify) logfl_qso_free);
  add_sqso (qsos, 1, "OK2XYZ", "20m", "CW", "15");  /* 0 pts, mults 15+OK */
  add_sqso (qsos, 2, "DL1AB", "20m", "CW", "14");   /* 1 pt,  14 + DL    */
  add_sqso (qsos, 3, "K1AB", "20m", "CW", "5");     /* 3 pts, 5 + K      */
  LogflContestTotals tot;
  GHashTable *scores =
    logfl_contest_score (def, cty, "OK1BR", qsos, &tot);
  g_assert_nonnull (scores);
  g_assert_cmpint (tot.points, ==, 4);
  g_assert_cmpint (tot.mults, ==, 6);
  g_assert_cmpint (tot.total, ==, 24);
  /* Zero-point own country still brought two multipliers — the CQ WW
   * trap the rules call out explicitly. */
  g_assert_cmpint (score_of (scores, 1)->points, ==, 0);
  g_assert_cmpstr (score_of (scores, 1)->mult, ==, "OK 15");

  g_hash_table_unref (scores);
  g_ptr_array_unref (qsos);
  logfl_cty_free (cty);
  logfl_exch_def_free (def);
}

static void
test_score_wpx (void)
{
  GError *err = NULL;
  LogflExchDef *def = logfl_exch_def_parse (
      "[exchange]\ntx_serial=true\nfields=nr;\n"
      "points=own-country=1;same-cont=1/2;other-cont=3/6;\n"
      "mult=prefix\nmult_scope=contest\n"
      "[field:nr]\nlabel=Nr\ntype=serial\n", &err);
  g_assert_no_error (err);

  LogflCty *cty = load_cty ();
  GPtrArray *qsos = g_ptr_array_new_with_free_func (
      (GDestroyNotify) logfl_qso_free);
  add_sqso (qsos, 1, "OK1AAA", "20m", "CW", NULL);  /* 1 pt, OK1        */
  add_sqso (qsos, 2, "OK1BBB", "40m", "CW", NULL);  /* 1 pt (flat), dup
                                                       OK1 contest-wide */
  add_sqso (qsos, 3, "DL1CC", "40m", "CW", NULL);   /* 2 pts (low), DL1 */
  add_sqso (qsos, 4, "K2DD", "40m", "CW", NULL);    /* 6 pts (low), K2  */
  add_sqso (qsos, 5, "K2EE", "20m", "CW", NULL);    /* 3 pts, K2 dup    */
  LogflContestTotals tot;
  GHashTable *scores =
    logfl_contest_score (def, cty, "OK1BR", qsos, &tot);
  g_assert_nonnull (scores);
  g_assert_cmpint (tot.points, ==, 13);
  g_assert_cmpint (tot.mults, ==, 3);
  g_assert_cmpint (tot.total, ==, 39);
  g_assert_null (score_of (scores, 2)->mult);
  g_assert_cmpstr (score_of (scores, 4)->mult, ==, "K2");

  g_hash_table_unref (scores);
  g_ptr_array_unref (qsos);
  logfl_cty_free (cty);
  logfl_exch_def_free (def);
}

static void
test_score_euhfc (void)
{
  GError *err = NULL;
  LogflExchDef *def = logfl_exch_def_parse (
      "[exchange]\nfields=year;\ncounts=eu-only\n"
      "points=default=1;\nmult=exch\n"
      "[field:year]\nlabel=Year\ntype=text\n", &err);
  g_assert_no_error (err);

  LogflCty *cty = load_cty ();
  GPtrArray *qsos = g_ptr_array_new_with_free_func (
      (GDestroyNotify) logfl_qso_free);
  add_sqso (qsos, 1, "OK1AAA", "80m", "CW", "82");  /* 1 pt, mult 82    */
  add_sqso (qsos, 2, "OK2BBB", "80m", "CW", "82");  /* 1 pt, 82 known   */
  add_sqso (qsos, 3, "DL1CC", "40m", "CW", "82");   /* 1 pt, 82 new band */
  add_sqso (qsos, 4, "K1AB", "20m", "CW", "99");    /* non-EU: nothing  */
  LogflContestTotals tot;
  GHashTable *scores =
    logfl_contest_score (def, cty, "OK1BR", qsos, &tot);
  g_assert_nonnull (scores);
  g_assert_cmpint (tot.points, ==, 3);
  g_assert_cmpint (tot.mults, ==, 2);
  g_assert_cmpint (tot.total, ==, 6);
  g_assert_cmpint (score_of (scores, 4)->points, ==, 0);
  g_assert_null (score_of (scores, 4)->mult);

  g_hash_table_unref (scores);
  g_ptr_array_unref (qsos);
  logfl_cty_free (cty);
  logfl_exch_def_free (def);
}

static void
test_score_wae (void)
{
  GError *err = NULL;
  LogflExchDef *def = logfl_exch_def_parse (
      "[exchange]\ntx_serial=true\nfields=nr;\ncounts=eu-dx\n"
      "points=default=1;\nmult=country-areas\n"
      "mult_weight=80m:4;40m:3;20m:2;15m:2;10m:2;\n"
      "[field:nr]\nlabel=Nr\ntype=serial\n", &err);
  g_assert_no_error (err);

  LogflCty *cty = load_cty ();
  GPtrArray *qsos = g_ptr_array_new_with_free_func (
      (GDestroyNotify) logfl_qso_free);
  add_sqso (qsos, 1, "K1AB", "80m", "CW", NULL);    /* 1 pt, K1 ×4      */
  add_sqso (qsos, 2, "K3CD/1", "80m", "CW", NULL);  /* 1 pt, K1 known   */
  add_sqso (qsos, 3, "JR4AAA", "40m", "CW", NULL);  /* 1 pt, JA4 ×3     */
  add_sqso (qsos, 4, "UA9ABC", "20m", "CW", NULL);  /* 1 pt, RA9 ×2     */
  add_sqso (qsos, 5, "DL1AB", "20m", "CW", NULL);   /* EU-EU: nothing   */
  LogflContestTotals tot;
  GHashTable *scores =
    logfl_contest_score (def, cty, "OK1BR", qsos, &tot);
  g_assert_nonnull (scores);
  g_assert_cmpint (tot.points, ==, 4);
  g_assert_cmpint (tot.mults, ==, 9);
  g_assert_cmpint (tot.total, ==, 36);
  g_assert_cmpstr (score_of (scores, 1)->mult, ==, "K1");
  g_assert_null (score_of (scores, 2)->mult);
  g_assert_cmpstr (score_of (scores, 4)->mult, ==, "RA9");
  g_assert_cmpint (score_of (scores, 5)->points, ==, 0);

  g_hash_table_unref (scores);
  g_ptr_array_unref (qsos);
  logfl_cty_free (cty);
  logfl_exch_def_free (def);
}

static void
test_score_iaru (void)
{
  GError *err = NULL;
  LogflExchDef *def = logfl_exch_def_parse (
      "[exchange]\nfields=exch;\n"
      "points=exch-text=1;same-zone=1;same-cont=3;other-cont=5;\n"
      "mult=zone+exch-text\n"
      "[field:exch]\nlabel=Zone/HQ\ntype=auto\nadif_num=ITUZ\n", &err);
  g_assert_no_error (err);

  LogflCty *cty = load_cty ();
  GPtrArray *qsos = g_ptr_array_new_with_free_func (
      (GDestroyNotify) logfl_qso_free);
  add_sqso (qsos, 1, "OM3AA", "20m", "CW", "28");   /* same zone: 1     */
  add_sqso (qsos, 2, "DL1AB", "20m", "CW", "27");   /* EU, diff zone: 3 */
  add_sqso (qsos, 3, "K1AB", "20m", "CW", "8");     /* other cont: 5    */
  add_sqso (qsos, 4, "DA0HQ", "20m", "SSB", "DARC");/* HQ text: 1       */
  LogflContestTotals tot;
  GHashTable *scores =
    logfl_contest_score (def, cty, "OK1BR", qsos, &tot);
  g_assert_nonnull (scores);
  g_assert_cmpint (tot.points, ==, 10);
  g_assert_cmpint (tot.mults, ==, 4);    /* zones 28 27 8 + DARC */
  g_assert_cmpint (tot.total, ==, 40);
  g_assert_cmpint (score_of (scores, 4)->points, ==, 1);
  g_assert_cmpstr (score_of (scores, 4)->mult, ==, "DARC");

  g_hash_table_unref (scores);
  g_ptr_array_unref (qsos);
  logfl_cty_free (cty);
  logfl_exch_def_free (def);
}

static void
test_score_unavailable (void)
{
  GError *err = NULL;
  LogflCty *cty = load_cty ();
  GPtrArray *qsos = g_ptr_array_new_with_free_func (
      (GDestroyNotify) logfl_qso_free);
  add_sqso (qsos, 1, "DL1AB", "20m", "CW", NULL);
  LogflContestTotals tot;

  /* No scoring rule → no score, never zeros pretending to be one. */
  LogflExchDef *plain = logfl_exch_def_parse (
      "[exchange]\nfields=nr;\n[field:nr]\nlabel=Nr\ntype=serial\n", &err);
  g_assert_no_error (err);
  g_assert_null (logfl_contest_score (plain, cty, "OK1BR", qsos, &tot));
  g_assert_false (tot.have_points);
  logfl_exch_def_free (plain);

  /* An unresolvable own seat cannot judge relations → also no score. */
  LogflExchDef *def = logfl_exch_def_parse (
      "[exchange]\nfields=nr;\npoints=own-country=1;\n"
      "[field:nr]\nlabel=Nr\ntype=serial\n", &err);
  g_assert_no_error (err);
  g_assert_null (logfl_contest_score (def, cty, NULL, qsos, &tot));
  g_assert_null (logfl_contest_score (def, cty, "1234", qsos, &tot));
  g_assert_null (logfl_contest_score (def, NULL, "OK1BR", qsos, &tot));
  logfl_exch_def_free (def);

  g_ptr_array_unref (qsos);
  logfl_cty_free (cty);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, >=, 2);   /* meson passes data/cty.dat */
  cty_path = argv[1];
  g_test_add_func ("/contest/exch-def-roundtrip", test_exch_def_roundtrip);
  g_test_add_func ("/contest/exch-def-errors", test_exch_def_errors);
  g_test_add_func ("/contest/qso-validity", test_qso_validity);
  g_test_add_func ("/contest/backfill-validity", test_backfill_validity);
  g_test_add_func ("/contest/presets", test_presets);
  g_test_add_func ("/contest/exch-apply", test_exch_apply);
  g_test_add_func ("/contest/store-crud", test_contest_crud);
  g_test_add_func ("/contest/migrate-v1", test_migrate_v1);
  g_test_add_func ("/contest/serial-dup-scoping", test_serial_dup_scoping);
  g_test_add_func ("/contest/delete-semantics", test_delete_semantics);
  g_test_add_func ("/contest/adif-roundtrip", test_adif_roundtrip);
  g_test_add_func ("/contest/adif-export-scoped", test_adif_export_scoped);
  g_test_add_func ("/contest/adif-serial-quirks", test_adif_serial_quirks);
  g_test_add_func ("/contest/score/roundtrip", test_score_roundtrip);
  g_test_add_func ("/contest/score/errors", test_score_errors);
  g_test_add_func ("/contest/score/wpx-prefix", test_wpx_prefix);
  g_test_add_func ("/contest/score/yodx", test_score_yodx);
  g_test_add_func ("/contest/score/cqww", test_score_cqww);
  g_test_add_func ("/contest/score/wpx", test_score_wpx);
  g_test_add_func ("/contest/score/euhfc", test_score_euhfc);
  g_test_add_func ("/contest/score/wae", test_score_wae);
  g_test_add_func ("/contest/score/iaru", test_score_iaru);
  g_test_add_func ("/contest/score/unavailable", test_score_unavailable);
  return g_test_run ();
}
