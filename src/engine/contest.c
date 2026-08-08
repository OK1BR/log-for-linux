/* contest.c — contest exchange templates (see contest.h).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "contest.h"

#include <string.h>

GQuark
logfl_contest_error_quark (void)
{
  return g_quark_from_static_string ("logfl-contest-error");
}

static void
exch_field_free (LogflExchField *f)
{
  g_free (f->key);
  g_free (f->label);
  g_free (f->adif_num);
  g_free (f->adif_text);
  g_free (f);
}

void
logfl_exch_def_free (LogflExchDef *def)
{
  if (!def)
    return;
  g_ptr_array_unref (def->fields);
  g_free (def);
}

/* ADIF field names: A-Z 0-9 _ after uppercasing, non-empty. */
static char *
adif_name_dup (const char *s, GError **error)
{
  char *up = g_ascii_strup (s, -1);
  g_strstrip (up);
  if (!*up)
    goto bad;
  for (const char *c = up; *c; c++)
    if (!g_ascii_isupper (*c) && !g_ascii_isdigit (*c) && *c != '_')
      goto bad;
  return up;

bad:
  g_set_error (error, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE,
               "\"%s\" is not an ADIF field name", s);
  g_free (up);
  return NULL;
}

static gboolean
type_from_string (const char *s, LogflExchFieldType *out)
{
  static const struct {
    const char *name;
    LogflExchFieldType type;
  } map[] = {
    { "serial", LOGFL_EXCH_SERIAL }, { "number", LOGFL_EXCH_NUMBER },
    { "text", LOGFL_EXCH_TEXT },     { "auto", LOGFL_EXCH_AUTO },
  };
  for (gsize i = 0; i < G_N_ELEMENTS (map); i++)
    if (g_strcmp0 (s, map[i].name) == 0)
      {
        *out = map[i].type;
        return TRUE;
      }
  return FALSE;
}

static const char *
type_to_string (LogflExchFieldType t)
{
  switch (t)
    {
    case LOGFL_EXCH_SERIAL: return "serial";
    case LOGFL_EXCH_NUMBER: return "number";
    case LOGFL_EXCH_TEXT:   return "text";
    case LOGFL_EXCH_AUTO:   return "auto";
    }
  return "auto";
}

LogflExchDef *
logfl_exch_def_parse (const char *text, GError **error)
{
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  if (!g_key_file_load_from_data (kf, text ? text : "", (gsize) -1,
                                  G_KEY_FILE_NONE, error))
    return NULL;
  if (!g_key_file_has_group (kf, "exchange"))
    {
      g_set_error (error, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE,
                   "exchange definition lacks the [exchange] group");
      return NULL;
    }

  LogflExchDef *def = g_new0 (LogflExchDef, 1);
  def->fields =
    g_ptr_array_new_with_free_func ((GDestroyNotify) exch_field_free);
  def->tx_serial =
    g_key_file_get_boolean (kf, "exchange", "tx_serial", NULL);
  def->zero_own_country =
    g_key_file_get_boolean (kf, "exchange", "zero_own_country", NULL);
  char *counts = g_key_file_get_string (kf, "exchange", "counts", NULL);
  if (counts)
    {
      g_strstrip (counts);
      if (g_str_equal (counts, "eu-dx"))
        def->counts = LOGFL_COUNTS_EU_DX;
      else if (g_str_equal (counts, "eu-only"))
        def->counts = LOGFL_COUNTS_EU_ONLY;
      else if (!g_str_equal (counts, "all") && *counts)
        {
          /* A rule this build does not know must not silently degrade to
           * "everything counts" — fail loud, like an unknown field type. */
          g_set_error (error, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE,
                       "unknown counts rule \"%s\"", counts);
          g_free (counts);
          logfl_exch_def_free (def);
          return NULL;
        }
      g_free (counts);
    }

  gsize n_keys = 0;
  char **keys =
    g_key_file_get_string_list (kf, "exchange", "fields", &n_keys, NULL);
  for (gsize i = 0; keys && i < n_keys; i++)
    {
      g_strstrip (keys[i]);
      if (!*keys[i])
        continue;
      char *group = g_strdup_printf ("field:%s", keys[i]);

      LogflExchField *f = g_new0 (LogflExchField, 1);
      g_ptr_array_add (def->fields, f);
      f->key = g_strdup (keys[i]);
      f->label = g_key_file_get_string (kf, group, "label", NULL);
      if (!f->label || !*g_strstrip (f->label))
        {
          g_free (f->label);
          f->label = g_strdup (keys[i]);
        }
      f->required = g_key_file_get_boolean (kf, group, "required", NULL);

      char *type = g_key_file_get_string (kf, group, "type", NULL);
      gboolean type_ok =
        !type || type_from_string (g_strstrip (type), &f->type);
      if (!type_ok)
        g_set_error (error, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE,
                     "field \"%s\" has unknown type \"%s\"", keys[i], type);
      g_free (type);

      char *num = g_key_file_get_string (kf, group, "adif_num", NULL);
      char *txt = g_key_file_get_string (kf, group, "adif_text", NULL);
      f->adif_num = !type_ok ? NULL
                  : num ? adif_name_dup (num, error) : g_strdup ("SRX");
      f->adif_text = !type_ok || !f->adif_num ? NULL
                   : txt ? adif_name_dup (txt, error)
                         : g_strdup ("SRX_STRING");
      g_free (num);
      g_free (txt);
      g_free (group);
      if (!type_ok || !f->adif_num || !f->adif_text)
        {
          g_strfreev (keys);
          logfl_exch_def_free (def);
          return NULL;
        }
    }
  g_strfreev (keys);
  return def;
}

char *
logfl_exch_def_serialize (const LogflExchDef *def)
{
  g_autoptr (GKeyFile) kf = g_key_file_new ();
  g_key_file_set_boolean (kf, "exchange", "tx_serial", def->tx_serial);
  g_key_file_set_string (kf, "exchange", "counts",
                         def->counts == LOGFL_COUNTS_EU_DX   ? "eu-dx"
                         : def->counts == LOGFL_COUNTS_EU_ONLY ? "eu-only"
                                                               : "all");
  g_key_file_set_boolean (kf, "exchange", "zero_own_country",
                          def->zero_own_country);

  GString *list = g_string_new (NULL);
  for (guint i = 0; i < def->fields->len; i++)
    {
      const LogflExchField *f = def->fields->pdata[i];
      g_string_append (list, f->key);
      g_string_append_c (list, ';');
    }
  g_key_file_set_value (kf, "exchange", "fields", list->str);
  g_string_free (list, TRUE);

  for (guint i = 0; i < def->fields->len; i++)
    {
      const LogflExchField *f = def->fields->pdata[i];
      char *group = g_strdup_printf ("field:%s", f->key);
      g_key_file_set_string (kf, group, "label", f->label);
      g_key_file_set_string (kf, group, "type", type_to_string (f->type));
      g_key_file_set_string (kf, group, "adif_num", f->adif_num);
      g_key_file_set_string (kf, group, "adif_text", f->adif_text);
      g_key_file_set_boolean (kf, group, "required", f->required);
      g_free (group);
    }
  return g_key_file_to_data (kf, NULL, NULL);
}

/* --- applying an exchange to a QSO -------------------------------------- */

static gboolean
all_digits (const char *s)
{
  if (!*s)
    return FALSE;
  for (; *s; s++)
    if (!g_ascii_isdigit (*s))
      return FALSE;
  return TRUE;
}

/* Multiple fields may share SRX_STRING (district + something); join on
 * space in field order rather than losing the earlier value. */
static void
join_str (char **slot, const char *val)
{
  if (!*slot)
    {
      *slot = g_strdup (val);
      return;
    }
  char *joined = g_strconcat (*slot, " ", val, NULL);
  g_free (*slot);
  *slot = joined;
}

static void
route_value (LogflQso *q, const char *target, const char *val)
{
  if (g_str_equal (target, "SRX"))
    q->srx = g_ascii_strtoll (val, NULL, 10);
  else if (g_str_equal (target, "STX"))
    q->stx = g_ascii_strtoll (val, NULL, 10);
  else if (g_str_equal (target, "SRX_STRING"))
    join_str (&q->srx_string, val);
  else if (g_str_equal (target, "STX_STRING"))
    join_str (&q->stx_string, val);
  else
    {
      /* Not a store column — ride in extras as a regular ADIF tag, the
       * same shape the ADIF writer emits (M2). Mirror the value into
       * SRX_STRING as well: exchange consumers (the Cabrillo writer, the
       * table's Rcvd column) read only the exchange columns, never extras,
       * so a zone routed solely to CQZ/ITUZ would silently vanish from a
       * submitted log. */
      join_str (&q->srx_string, val);
      GString *e = g_string_new (q->extras);
      g_string_append_printf (e, "<%s:%zu>%s", target, strlen (val), val);
      g_free (q->extras);
      q->extras = g_string_free (e, FALSE);
    }
}

void
logfl_exch_apply (const LogflExchDef *def,
                  const char *const *values, guint n_values,
                  const char *my_exch, guint serial,
                  LogflQso *qso)
{
  if (def->tx_serial && serial > 0)
    qso->stx = serial;
  if (my_exch)
    {
      char *mine = g_strstrip (g_ascii_strup (my_exch, -1));
      if (*mine)
        join_str (&qso->stx_string, mine);
      g_free (mine);
    }

  guint n = MIN (n_values, def->fields->len);
  for (guint i = 0; i < n; i++)
    {
      const LogflExchField *f = def->fields->pdata[i];
      if (!values[i])
        continue;
      char *v = g_strstrip (g_ascii_strup (values[i], -1));
      if (*v)
        {
          gboolean numeric =
            f->type != LOGFL_EXCH_TEXT && all_digits (v);
          route_value (qso, numeric ? f->adif_num : f->adif_text, v);
        }
      g_free (v);
    }
}

char *
logfl_exch_serial_format (guint serial)
{
  return g_strdup_printf ("%03u", serial);
}

LogflQsoValidity
logfl_contest_qso_validity (const LogflExchDef *def,
                            const LogflCtyInfo *mine,
                            const LogflCtyInfo *theirs)
{
  /* Unresolved calls never alarm: a cty miss is our gap, not the op's. */
  if (!def || !theirs || !theirs->country)
    return LOGFL_QSO_VALID;

  switch (def->counts)
    {
    case LOGFL_COUNTS_EU_DX:
      /* The QSO must cross the EU boundary — judged from my side when
       * known (OK: EU stations do not count), from theirs alone when
       * my station is unresolved. */
      if (mine && mine->country &&
          (g_str_equal (mine->continent, "EU") ==
           g_str_equal (theirs->continent, "EU")))
        return LOGFL_QSO_NOT_VALID;
      break;
    case LOGFL_COUNTS_EU_ONLY:
      if (!g_str_equal (theirs->continent, "EU"))
        return LOGFL_QSO_NOT_VALID;
      break;
    case LOGFL_COUNTS_ALL:
    default:
      break;
    }

  if (def->zero_own_country && mine && mine->country &&
      g_str_equal (mine->prefix, theirs->prefix))
    return LOGFL_QSO_ZERO_POINTS;
  return LOGFL_QSO_VALID;
}

/* --- presets ------------------------------------------------------------ */

/* Validity rules (counts= / zero_own_country) were verified against the
 * sponsors' official rules on 2026-08-08 — house rule: never add a preset
 * without that check. Sources sit at each preset. */
static const LogflContestPreset presets[] = {
  /* cqww.com/rules.htm: everyone works everyone; a same-country QSO scores
   * zero points but still counts for zone/country multipliers. */
  { "CQ WW", "CQ-WW",
    "[exchange]\ntx_serial=false\nfields=zone;\nzero_own_country=true\n"
    "[field:zone]\nlabel=Zone\ntype=number\nadif_num=CQZ\nrequired=true\n",
    "CQ zone (OK = 15)" },
  /* cqwpx.com/rules: everyone works everyone, every QSO >= 1 point. */
  { "CQ WPX", "CQ-WPX",
    "[exchange]\ntx_serial=true\nfields=nr;\n"
    "[field:nr]\nlabel=Nr\ntype=serial\nrequired=true\n",
    NULL },
  /* IARU-HF-Rules.pdf (contests.arrl.org): "Any station may be contacted
   * on each band", points 1/3/5 — nothing scores zero. */
  { "IARU HF", "IARU-HF",
    "[exchange]\ntx_serial=false\nfields=exch;\n"
    "[field:exch]\nlabel=Zone/HQ\ntype=auto\nadif_num=ITUZ\nrequired=true\n",
    "ITU zone (OK = 28)" },
  /* okomdx.crk.cz CW rules (2026): everyone works everyone — the old
   * OK/OM-with-foreigners-only restriction is gone; an OK station scores
   * even OK-OK (2 points). NB the CRC war statement: QSOs with RU/BY
   * stations are annulled — not modeled here (temporary, "until further
   * notice"). */
  { "OK/OM DX", "OK-OM-DX",
    "[exchange]\ntx_serial=false\nfields=exch;\n"
    "[field:exch]\nlabel=Nr/District\ntype=auto\nrequired=true\n",
    "district (e.g. APA)" },
  /* Exchange is the two-digit year of the operator's FIRST licence
   * (euhf.s5cc.eu rules) — text, so "01" (= 2001) keeps its zero and the
   * multiplier string survives verbatim. Once per band and mode = exactly
   * the app's contest dup rule. Rules (PDF, updated 2026-06-30): "Only
   * continental Europe contacts count" — hence counts=eu-only. */
  { "EUHFC", "EU-HF",
    "[exchange]\ntx_serial=false\nfields=year;\ncounts=eu-only\n"
    "[field:year]\nlabel=Year\ntype=text\nrequired=true\n",
    "two-digit year of first licence (e.g. 99)" },
  /* WAE (darc.de rules): RST + progressive serial, "000" when the other
   * side sends none; "a contest QSO can only be conducted between a
   * European and a non-European station" — counts=eu-dx. The ADIF id
   * is per part — the CW prefill matches the August edition, SSB/RTTY
   * editions edit the suffix. Cabrillo CONTEST equals the ADIF id per the
   * WA7BNM master list; DARC's own rules name no value. */
  { "WAE DX", "DARC-WAEDC-CW",
    "[exchange]\ntx_serial=true\nfields=nr;\ncounts=eu-dx\n"
    "[field:nr]\nlabel=Nr\ntype=serial\nrequired=true\n",
    NULL },
  /* CVA (cvadx.org/regulamento): we send RS(T) + continent ("599 EU"); PY
   * stations answer with their state — both two-letter TEXT, never a
   * serial. Everyone works everyone (2/3/4 points by distance) — not a
   * PY-only contest despite the exchange shape. SSB edition edits the
   * ADIF id suffix. */
  { "CVA DX", "CVA-DX-CW",
    "[exchange]\ntx_serial=false\nfields=cont;\n"
    "[field:cont]\nlabel=Cont/State\ntype=text\nrequired=true\n",
    "continent (OK = EU)" },
  /* SARTG WW RTTY (sartg.com/contest/wwrules.htm): everyone works
   * everyone, 5/10/15 points; the rules themselves fix the Cabrillo
   * CONTEST name to SARTG-RTTY. */
  { "SARTG WW RTTY", "SARTG-RTTY",
    "[exchange]\ntx_serial=true\nfields=nr;\n"
    "[field:nr]\nlabel=Nr\ntype=serial\nrequired=true\n",
    NULL },
  { "Custom", NULL,
    "[exchange]\ntx_serial=false\nfields=exch;\n"
    "[field:exch]\nlabel=Exch\ntype=auto\nrequired=false\n",
    NULL },
};

const LogflContestPreset *
logfl_contest_presets (guint *n)
{
  *n = G_N_ELEMENTS (presets);
  return presets;
}
