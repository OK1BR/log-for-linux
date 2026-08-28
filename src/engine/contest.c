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
  if (def->points)
    {
      for (guint i = 0; i < def->points->len; i++)
        g_free (g_array_index (def->points, LogflPtsTerm, i).arg);
      g_array_unref (def->points);
    }
  g_free (def->mult_exch_from);
  g_clear_pointer (&def->mult_weight, g_hash_table_unref);
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

/* points= term list: "name[:ARG]=N[/L];…", ordered, first match wins. */
static gboolean
pts_parse (LogflExchDef *def, const char *value, GError **error)
{
  static const struct {
    const char *name;
    LogflPtsKind kind;
    gboolean has_arg;
  } kinds[] = {
    { "country",     LOGFL_PTS_COUNTRY,     TRUE  },
    { "own-country", LOGFL_PTS_OWN_COUNTRY, FALSE },
    { "same-cont",   LOGFL_PTS_SAME_CONT,   FALSE },
    { "other-cont",  LOGFL_PTS_OTHER_CONT,  FALSE },
    { "same-zone",   LOGFL_PTS_SAME_ZONE,   FALSE },
    { "exch-text",   LOGFL_PTS_EXCH_TEXT,   FALSE },
    { "default",     LOGFL_PTS_DEFAULT,     FALSE },
  };

  def->points = g_array_new (FALSE, TRUE, sizeof (LogflPtsTerm));
  char **terms = g_strsplit (value, ";", -1);
  for (char **t = terms; *t; t++)
    {
      g_strstrip (*t);
      if (!**t)
        continue;
      char *eq = strchr (*t, '=');
      if (!eq)
        goto bad;
      *eq = '\0';
      char *name = *t, *arg = strchr (name, ':');
      if (arg)
        *arg++ = '\0';

      gsize k = 0;
      while (k < G_N_ELEMENTS (kinds) && !g_str_equal (name, kinds[k].name))
        k++;
      if (k == G_N_ELEMENTS (kinds) || (arg != NULL) != kinds[k].has_arg)
        goto bad;

      LogflPtsTerm term = { .kind = kinds[k].kind,
                            .arg = arg ? g_ascii_strup (arg, -1) : NULL };
      char *slash = NULL;
      term.points = (int) g_ascii_strtoll (eq + 1, &slash, 10);
      term.points_low =
        slash && *slash == '/' ? (int) g_ascii_strtoll (slash + 1, NULL, 10)
                               : term.points;
      g_array_append_val (def->points, term);
      continue;

    bad:
      g_set_error (error, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE,
                   "unknown points term \"%s\"", *t);
      g_strfreev (terms);
      return FALSE;
    }
  g_strfreev (terms);
  return TRUE;
}

/* mult= source list: "country|country-areas|cqzone|zone|exch[:PFX]|
 * exch-text|prefix", joined with '+'. */
static gboolean
mult_parse (LogflExchDef *def, const char *value, GError **error)
{
  char **toks = g_strsplit (value, "+", -1);
  for (char **t = toks; *t; t++)
    {
      g_strstrip (*t);
      if (g_str_equal (*t, "country"))
        def->mult |= LOGFL_MULT_COUNTRY;
      else if (g_str_equal (*t, "country-areas"))
        def->mult |= LOGFL_MULT_COUNTRY_AREAS;
      else if (g_str_equal (*t, "cqzone"))
        def->mult |= LOGFL_MULT_CQZONE;
      else if (g_str_equal (*t, "zone"))
        def->mult |= LOGFL_MULT_ZONE;
      else if (g_str_equal (*t, "exch-text"))
        def->mult |= LOGFL_MULT_EXCH_TEXT;
      else if (g_str_has_prefix (*t, "exch"))
        {
          def->mult |= LOGFL_MULT_EXCH;
          if ((*t)[4] == ':')
            def->mult_exch_from = g_ascii_strup (*t + 5, -1);
          else if ((*t)[4] != '\0')
            goto bad;
        }
      else if (g_str_equal (*t, "prefix"))
        def->mult |= LOGFL_MULT_PREFIX;
      else
        {
        bad:
          g_set_error (error, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE,
                       "unknown mult source \"%s\"", *t);
          g_strfreev (toks);
          return FALSE;
        }
    }
  g_strfreev (toks);
  return TRUE;
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

  char *pts = g_key_file_get_string (kf, "exchange", "points", NULL);
  if (pts)
    {
      gboolean ok = pts_parse (def, pts, error);
      g_free (pts);
      if (!ok)
        {
          logfl_exch_def_free (def);
          return NULL;
        }
    }
  char *mult = g_key_file_get_string (kf, "exchange", "mult", NULL);
  if (mult)
    {
      gboolean ok = mult_parse (def, mult, error);
      g_free (mult);
      if (!ok)
        {
          logfl_exch_def_free (def);
          return NULL;
        }
    }
  char *scope = g_key_file_get_string (kf, "exchange", "mult_scope", NULL);
  if (scope)
    {
      g_strstrip (scope);
      if (g_str_equal (scope, "contest"))
        def->mult_per_contest = TRUE;
      else if (!g_str_equal (scope, "band") && *scope)
        {
          g_set_error (error, LOGFL_CONTEST_ERROR, LOGFL_CONTEST_ERROR_PARSE,
                       "unknown mult scope \"%s\"", scope);
          g_free (scope);
          logfl_exch_def_free (def);
          return NULL;
        }
      g_free (scope);
    }
  char *weight = g_key_file_get_string (kf, "exchange", "mult_weight", NULL);
  if (weight)
    {
      def->mult_weight =
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      char **toks = g_strsplit (weight, ";", -1);
      for (char **t = toks; *t; t++)
        {
          g_strstrip (*t);
          if (!**t)
            continue;
          char *colon = strrchr (*t, ':');
          gint64 w = colon ? g_ascii_strtoll (colon + 1, NULL, 10) : 0;
          if (!colon || w <= 0)
            {
              g_set_error (error, LOGFL_CONTEST_ERROR,
                           LOGFL_CONTEST_ERROR_PARSE,
                           "bad mult weight \"%s\"", *t);
              g_strfreev (toks);
              g_free (weight);
              logfl_exch_def_free (def);
              return NULL;
            }
          *colon = '\0';
          g_hash_table_insert (def->mult_weight, g_strdup (*t),
                               GINT_TO_POINTER ((int) w));
        }
      g_strfreev (toks);
      g_free (weight);
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

  if (def->points)
    {
      static const char *names[] = {
        [LOGFL_PTS_COUNTRY] = "country", [LOGFL_PTS_OWN_COUNTRY] = "own-country",
        [LOGFL_PTS_SAME_CONT] = "same-cont", [LOGFL_PTS_OTHER_CONT] = "other-cont",
        [LOGFL_PTS_SAME_ZONE] = "same-zone", [LOGFL_PTS_EXCH_TEXT] = "exch-text",
        [LOGFL_PTS_DEFAULT] = "default",
      };
      GString *s = g_string_new (NULL);
      for (guint i = 0; i < def->points->len; i++)
        {
          const LogflPtsTerm *t = &g_array_index (def->points, LogflPtsTerm, i);
          g_string_append (s, names[t->kind]);
          if (t->arg)
            g_string_append_printf (s, ":%s", t->arg);
          g_string_append_printf (s, "=%d", t->points);
          if (t->points_low != t->points)
            g_string_append_printf (s, "/%d", t->points_low);
          g_string_append_c (s, ';');
        }
      g_key_file_set_value (kf, "exchange", "points", s->str);
      g_string_free (s, TRUE);
    }
  if (def->mult)
    {
      GString *s = g_string_new (NULL);
      static const struct { guint bit; const char *name; } srcs[] = {
        { LOGFL_MULT_COUNTRY, "country" },
        { LOGFL_MULT_COUNTRY_AREAS, "country-areas" },
        { LOGFL_MULT_CQZONE, "cqzone" }, { LOGFL_MULT_ZONE, "zone" },
        { LOGFL_MULT_EXCH, "exch" }, { LOGFL_MULT_EXCH_TEXT, "exch-text" },
        { LOGFL_MULT_PREFIX, "prefix" },
      };
      for (gsize i = 0; i < G_N_ELEMENTS (srcs); i++)
        {
          if (!(def->mult & srcs[i].bit))
            continue;
          if (s->len)
            g_string_append_c (s, '+');
          g_string_append (s, srcs[i].name);
          if (srcs[i].bit == LOGFL_MULT_EXCH && def->mult_exch_from)
            g_string_append_printf (s, ":%s", def->mult_exch_from);
        }
      g_key_file_set_value (kf, "exchange", "mult", s->str);
      g_string_free (s, TRUE);
      if (def->mult_per_contest)
        g_key_file_set_string (kf, "exchange", "mult_scope", "contest");
      if (def->mult_weight)
        {
          GString *w = g_string_new (NULL);
          GHashTableIter it;
          gpointer band, weight;
          g_hash_table_iter_init (&it, def->mult_weight);
          while (g_hash_table_iter_next (&it, &band, &weight))
            g_string_append_printf (w, "%s:%d;", (const char *) band,
                                    GPOINTER_TO_INT (weight));
          g_key_file_set_value (kf, "exchange", "mult_weight", w->str);
          g_string_free (w, TRUE);
        }
    }

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

/* --- scoring (LOG-3) ---------------------------------------------------- */

/* Split "PA/N8BJQ", "K5DJ/1", "OK1BR/P": the longest call-like token is
 * the call, a remaining non-suffix token the portable designator. Both
 * outputs upper-cased; NULL when absent. */
static void
call_split (const char *call, char **main_out, char **desig_out)
{
  *main_out = *desig_out = NULL;
  char *up = g_strstrip (g_ascii_strup (call, -1));
  char **tok = g_strsplit (up, "/", -1);
  g_free (up);

  static const char *const suffixes[] =
    { "P", "M", "MM", "AM", "QRP", "A", "E", "J", NULL };
  for (char **t = tok; *t; t++)
    {
      if (!**t)
        continue;
      gboolean skip = FALSE;
      for (gsize i = 0; suffixes[i]; i++)
        if (g_str_equal (*t, suffixes[i]))
          skip = TRUE;
      /* A bare digit is a call-area designator, never the call. */
      if (strlen (*t) == 1 && g_ascii_isdigit (**t))
        {
          g_free (*desig_out);
          *desig_out = g_strdup (*t);
          continue;
        }
      if (skip)
        continue;
      if (!*main_out || strlen (*t) > strlen (*main_out))
        {
          if (*main_out)
            {
              g_free (*desig_out);
              *desig_out = *main_out;
            }
          *main_out = g_strdup (*t);
        }
      else
        {
          g_free (*desig_out);
          *desig_out = g_strdup (*t);
        }
    }
  g_strfreev (tok);
}

/* "OK1BR"→"OK1", "LY1000XX"→"LY1000": up to and including the last digit;
 * no digit → first two characters + "0" (WPX rules §V.C). */
static char *
token_prefix (const char *tok)
{
  const char *last = NULL;
  for (const char *c = tok; *c; c++)
    if (g_ascii_isdigit (*c))
      last = c;
  if (last)
    return g_strndup (tok, (gsize) (last - tok) + 1);
  return g_strdup_printf ("%.2s0", tok);
}

char *
logfl_wpx_prefix (const char *call)
{
  if (!call || !*call)
    return NULL;
  char *main_tok = NULL, *desig = NULL;
  call_split (call, &main_tok, &desig);
  if (!main_tok)
    {
      g_free (desig);
      return NULL;
    }

  char *pfx;
  if (desig && strlen (desig) == 1 && g_ascii_isdigit (*desig))
    {
      /* Digit designator swaps the call area: K5DJ/1 → K1. */
      char *base = token_prefix (main_tok);
      char *end = base + strlen (base);
      while (end > base && g_ascii_isdigit (end[-1]))
        *--end = '\0';
      pfx = g_strdup_printf ("%s%s", base, desig);
      g_free (base);
    }
  else if (desig)
    pfx = token_prefix (desig);
  else
    pfx = token_prefix (main_tok);
  g_free (main_tok);
  g_free (desig);
  return pfx;
}

/* Numerical call area: a bare digit designator wins (K5DJ/1 → 1), else
 * the last digit of the call's prefix part (JR4ABC → 4); '\0' if none. */
static char
call_area_digit (const char *call)
{
  char *main_tok = NULL, *desig = NULL;
  call_split (call, &main_tok, &desig);
  char d = '\0';
  if (desig && strlen (desig) == 1 && g_ascii_isdigit (*desig))
    d = *desig;
  else if (main_tok)
    {
      const char *last = NULL;
      for (const char *c = main_tok; *c; c++)
        if (g_ascii_isdigit (*c))
          last = c;
      if (last)
        d = *last;
    }
  g_free (main_tok);
  g_free (desig);
  return d;
}

/* WAE DX rules §6: in these entities up to ten numerical call areas count
 * as separate multipliers; elsewhere the entity itself is the multiplier.
 * cty primary prefixes; the UA9 areas are named RA8/RA9/RA0 by the rules. */
static char *
mult_country_area (const LogflCtyInfo *theirs, const char *call)
{
  static const char *const split[] =
    { "K", "VE", "VK", "ZL", "ZS", "JA", "BY", "PY", "UA9", NULL };
  gsize i = 0;
  while (split[i] && !g_str_equal (theirs->prefix, split[i]))
    i++;
  char d = split[i] ? call_area_digit (call) : '\0';
  if (!d)
    return g_strdup (theirs->prefix);
  if (g_str_equal (theirs->prefix, "UA9"))
    return g_strdup_printf ("RA%c", d);
  return g_strdup_printf ("%s%c", theirs->prefix, d);
}

/* WPX low bands (§V.B: 7, 3.5 and 1.8 MHz score double). */
static gboolean
band_is_low (const char *band)
{
  return band && (g_str_equal (band, "160m") || g_str_equal (band, "80m")
                  || g_str_equal (band, "40m"));
}

/* Their zone: the received exchange when numeric — the station said it
 * itself — the cty default otherwise (0 = unknown). */
static int
their_zone (const LogflQso *q, gboolean theirs_ok,
            const LogflCtyInfo *theirs, gboolean cq)
{
  if (q->srx_string && all_digits (q->srx_string))
    return (int) g_ascii_strtoll (q->srx_string, NULL, 10);
  if (theirs_ok)
    return cq ? theirs->cq_zone : theirs->itu_zone;
  return 0;
}

static int
pts_eval (const LogflExchDef *def, const LogflQso *q,
          const LogflCtyInfo *mine, gboolean theirs_ok,
          const LogflCtyInfo *theirs)
{
  gboolean low = band_is_low (q->band);
  for (guint i = 0; i < def->points->len; i++)
    {
      const LogflPtsTerm *t = &g_array_index (def->points, LogflPtsTerm, i);
      gboolean match = FALSE;
      switch (t->kind)
        {
        case LOGFL_PTS_COUNTRY:
          match = theirs_ok && g_str_equal (theirs->prefix, t->arg);
          break;
        case LOGFL_PTS_OWN_COUNTRY:
          match = theirs_ok && g_str_equal (theirs->prefix, mine->prefix);
          break;
        case LOGFL_PTS_SAME_CONT:
          match =
            theirs_ok && g_str_equal (theirs->continent, mine->continent);
          break;
        case LOGFL_PTS_OTHER_CONT:
          match =
            theirs_ok && !g_str_equal (theirs->continent, mine->continent);
          break;
        case LOGFL_PTS_SAME_ZONE:
          {
            int tz = their_zone (q, theirs_ok, theirs, FALSE);
            match = tz > 0 && mine->itu_zone > 0 && tz == mine->itu_zone;
          }
          break;
        case LOGFL_PTS_EXCH_TEXT:
          match = q->srx_string && *q->srx_string
                  && !all_digits (q->srx_string);
          break;
        case LOGFL_PTS_DEFAULT:
          match = TRUE;
          break;
        }
      if (match)
        return low ? t->points_low : t->points;
    }
  return 0;                    /* an unresolved call matches no relation */
}

/* One multiplier candidate: count it if its (band-scoped) key is new,
 * crediting the band weight and appending the label. tag namespaces the
 * source — a YO county "CT" and Portugal's prefix "CT" are two different
 * multipliers and must never swallow each other. */
static void
mult_take (const LogflExchDef *def, const char *band, const char *tag,
           const char *key, GHashTable *seen, GString *label,
           gint64 *mults)
{
  char *scoped = def->mult_per_contest
                   ? g_strdup_printf ("%s|%s", tag, key)
                   : g_strdup_printf ("%s|%s|%s", band ? band : "", tag,
                                      key);
  if (g_hash_table_contains (seen, scoped))
    {
      g_free (scoped);
      return;
    }
  g_hash_table_add (seen, scoped);
  int w = 1;
  if (def->mult_weight && !def->mult_per_contest && band)
    {
      gpointer p = g_hash_table_lookup (def->mult_weight, band);
      if (p)
        w = GPOINTER_TO_INT (p);
    }
  *mults += w;
  if (label->len)
    g_string_append_c (label, ' ');
  g_string_append (label, key);
}

static void
mult_eval (const LogflExchDef *def, const LogflQso *q,
           gboolean theirs_ok, const LogflCtyInfo *theirs,
           GHashTable *seen, GString *label, gint64 *mults)
{
  if ((def->mult & LOGFL_MULT_COUNTRY) && theirs_ok)
    mult_take (def, q->band, "c", theirs->prefix, seen, label, mults);
  if ((def->mult & LOGFL_MULT_COUNTRY_AREAS) && theirs_ok)
    {
      char *key = mult_country_area (theirs, q->call);
      mult_take (def, q->band, "c", key, seen, label, mults);
      g_free (key);
    }
  if (def->mult & (LOGFL_MULT_CQZONE | LOGFL_MULT_ZONE))
    {
      int z = their_zone (q, theirs_ok, theirs,
                          (def->mult & LOGFL_MULT_CQZONE) != 0);
      if (z > 0)
        {
          char *key = g_strdup_printf ("%d", z);
          mult_take (def, q->band, "z", key, seen, label, mults);
          g_free (key);
        }
    }
  if (q->srx_string && *q->srx_string)
    {
      if ((def->mult & LOGFL_MULT_EXCH)
          && (!def->mult_exch_from
              || (theirs_ok
                  && g_str_equal (theirs->prefix, def->mult_exch_from))))
        {
          char *key = g_ascii_strup (q->srx_string, -1);
          mult_take (def, q->band, "e", g_strstrip (key), seen, label,
                     mults);
          g_free (key);
        }
      if ((def->mult & LOGFL_MULT_EXCH_TEXT)
          && !all_digits (q->srx_string))
        {
          char *key = g_ascii_strup (q->srx_string, -1);
          mult_take (def, q->band, "e", g_strstrip (key), seen, label,
                     mults);
          g_free (key);
        }
    }
  if (def->mult & LOGFL_MULT_PREFIX)
    {
      char *key = logfl_wpx_prefix (q->call);
      if (key)
        {
          mult_take (def, q->band, "p", key, seen, label, mults);
          g_free (key);
        }
    }
}

static void
qso_score_free (gpointer p)
{
  LogflQsoScore *sc = p;
  g_free (sc->mult);
  g_free (sc);
}

GHashTable *
logfl_contest_score (const LogflExchDef *def, LogflCty *cty,
                     const char *my_call, const GPtrArray *qsos,
                     LogflContestTotals *totals)
{
  memset (totals, 0, sizeof *totals);
  if (!def || (!def->points && !def->mult))
    return NULL;
  LogflCtyInfo mine;
  if (!cty || !my_call || !*my_call
      || !logfl_cty_lookup (cty, my_call, &mine))
    return NULL;
  totals->have_points = def->points != NULL;
  totals->have_mult = def->mult != 0;

  GHashTable *scores = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                              g_free, qso_score_free);
  GHashTable *dup =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *seen =
    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  /* The store lists newest first — walk oldest first, so "first QSO" and
   * "new multiplier" mean what they meant on the air. */
  for (guint i = qsos ? qsos->len : 0; i > 0; i--)
    {
      const LogflQso *q = qsos->pdata[i - 1];
      LogflQsoScore *sc = g_new0 (LogflQsoScore, 1);
      gint64 *k = g_new (gint64, 1);
      *k = q->id;
      g_hash_table_replace (scores, k, sc);

      if (!q->call || !*q->call)
        continue;
      char *dupkey = g_strdup_printf ("%s|%s|%s", q->call,
                                      q->band ? q->band : "",
                                      q->mode ? q->mode : "");
      if (g_hash_table_contains (dup, dupkey))
        {
          g_free (dupkey);       /* dupe: 0 points, no multiplier */
          continue;
        }
      g_hash_table_add (dup, dupkey);

      LogflCtyInfo theirs;
      gboolean theirs_ok = logfl_cty_lookup (cty, q->call, &theirs);
      LogflQsoValidity v =
        logfl_contest_qso_validity (def, &mine, theirs_ok ? &theirs : NULL);
      if (v == LOGFL_QSO_NOT_VALID)
        continue;

      if (def->points && v != LOGFL_QSO_ZERO_POINTS)
        {
          sc->points = pts_eval (def, q, &mine, theirs_ok, &theirs);
          totals->points += sc->points;
        }
      if (def->mult)
        {
          GString *label = g_string_new (NULL);
          mult_eval (def, q, theirs_ok, &theirs, seen, label,
                     &totals->mults);
          if (label->len)
            sc->mult = g_string_free (label, FALSE);
          else
            g_string_free (label, TRUE);
        }
    }
  g_hash_table_unref (dup);
  g_hash_table_unref (seen);

  totals->total = !totals->have_points ? totals->mults
                : totals->have_mult    ? totals->points * totals->mults
                                       : totals->points;
  return scores;
}

/* Scoring rule text per preset — shared between the preset definitions
 * below and the backfill table, so a contest created before LOG-3 gets
 * exactly the rule a fresh preset copy would carry. Sources at the
 * presets; all verified 2026-08-28. */
#define PTS_CQWW   "own-country=0;same-cont=1;other-cont=3;"
#define MULT_CQWW  "cqzone+country"
#define PTS_WPX    "own-country=1;same-cont=1/2;other-cont=3/6;"
#define MULT_WPX   "prefix"
#define PTS_IARU   "exch-text=1;same-zone=1;same-cont=3;other-cont=5;"
#define MULT_IARU  "zone+exch-text"
#define PTS_OKOM   "own-country=2;same-cont=3;other-cont=5;"
#define MULT_OKOM  "exch+country"
#define PTS_EUHFC  "default=1;"
#define MULT_EUHFC "exch"
#define PTS_WAE    "default=1;"
#define MULT_WAE   "country-areas"
#define WEIGHT_WAE "80m:4;40m:3;20m:2;15m:2;10m:2;"
#define PTS_CVA    "own-country=2;same-cont=3;other-cont=4;"
#define MULT_CVA   "exch:PY+country"
#define PTS_SARTG  "own-country=5;same-cont=10;other-cont=15;"
#define PTS_YODX   "country:YO=8;own-country=1;same-cont=2;other-cont=4;"
#define MULT_YODX  "exch:YO+country"

guint
logfl_contest_backfill_validity (LogflStore *s, GError **error)
{
  static const struct {
    const char *adif_prefix;
    const char *counts;          /* NULL = leave counts at "all" */
    gboolean    zero_own;
    const char *points;          /* LOG-3 scoring; NULL = none known */
    const char *mult;
    const char *scope;
    const char *weight;
  } rules[] = {
    { "DARC-WAEDC", "eu-dx",   FALSE, PTS_WAE,   MULT_WAE,   NULL, WEIGHT_WAE },
    { "EU-HF",      "eu-only", FALSE, PTS_EUHFC, MULT_EUHFC, NULL, NULL },
    { "CQ-WW",      NULL,      TRUE,  PTS_CQWW,  MULT_CQWW,  NULL, NULL },
    { "CQ-WPX",     NULL,      FALSE, PTS_WPX,   MULT_WPX,   "contest", NULL },
    { "IARU-HF",    NULL,      FALSE, PTS_IARU,  MULT_IARU,  NULL, NULL },
    { "OK-OM-DX",   NULL,      FALSE, PTS_OKOM,  MULT_OKOM,  NULL, NULL },
    { "CVA-DX",     NULL,      FALSE, PTS_CVA,   MULT_CVA,   NULL, NULL },
    { "SARTG-RTTY", NULL,      FALSE, PTS_SARTG, NULL,       NULL, NULL },
    { "YOHFDX",     NULL,      FALSE, PTS_YODX,  MULT_YODX,  NULL, NULL },
  };

  GPtrArray *list = logfl_store_contest_list (s, error);
  if (!list)
    return 0;

  guint n = 0;
  for (guint i = 0; i < list->len; i++)
    {
      LogflContest *c = list->pdata[i];
      if (!c->adif_id || !c->exch_def)
        continue;
      gsize r = 0;
      while (r < G_N_ELEMENTS (rules) &&
             !g_str_has_prefix (c->adif_id, rules[r].adif_prefix))
        r++;
      if (r == G_N_ELEMENTS (rules))
        continue;

      /* Edit the keyfile in place — a parse/serialize round trip would
       * drop keys this build does not know. Each backfill applies only
       * while the def names none of its keys: an operator's own edit —
       * including an explicit counts=all — is never overridden. */
      g_autoptr (GKeyFile) kf = g_key_file_new ();
      if (!g_key_file_load_from_data (kf, c->exch_def, (gsize) -1,
                                      G_KEY_FILE_NONE, NULL) ||
          !g_key_file_has_group (kf, "exchange"))
        continue;

      gboolean want_validity =
        (rules[r].counts || rules[r].zero_own) &&
        !g_key_file_has_key (kf, "exchange", "counts", NULL) &&
        !g_key_file_has_key (kf, "exchange", "zero_own_country", NULL);
      gboolean want_scoring =
        (rules[r].points || rules[r].mult) &&
        !g_key_file_has_key (kf, "exchange", "points", NULL) &&
        !g_key_file_has_key (kf, "exchange", "mult", NULL);
      if (!want_validity && !want_scoring)
        continue;

      if (want_validity)
        {
          if (rules[r].counts)
            g_key_file_set_string (kf, "exchange", "counts",
                                   rules[r].counts);
          if (rules[r].zero_own)
            g_key_file_set_boolean (kf, "exchange", "zero_own_country",
                                    TRUE);
        }
      if (want_scoring)
        {
          if (rules[r].points)
            g_key_file_set_value (kf, "exchange", "points",
                                  rules[r].points);
          if (rules[r].mult)
            g_key_file_set_value (kf, "exchange", "mult", rules[r].mult);
          if (rules[r].scope)
            g_key_file_set_string (kf, "exchange", "mult_scope",
                                   rules[r].scope);
          if (rules[r].weight)
            g_key_file_set_value (kf, "exchange", "mult_weight",
                                  rules[r].weight);
        }
      g_free (c->exch_def);
      c->exch_def = g_key_file_to_data (kf, NULL, NULL);
      if (!logfl_store_contest_update (s, c, error))
        break;
      n++;
    }
  g_ptr_array_unref (list);
  return n;
}

/* --- presets ------------------------------------------------------------ */

/* Validity rules (counts= / zero_own_country) were verified against the
 * sponsors' official rules on 2026-08-08 — house rule: never add a preset
 * without that check. Sources sit at each preset. Scoring rules (points= /
 * mult=) verified the same way on 2026-08-28, always from an OK seat —
 * clauses that cannot apply to an OK entry (CQ WW/WPX NA bonus, YO-side
 * rules) are not modeled, same as the validity rules before them. */
static const LogflContestPreset presets[] = {
  /* cqww.com/rules.htm: everyone works everyone; a same-country QSO scores
   * zero points but still counts for zone/country multipliers. Scoring §IV
   * (2026-08-28): 0 own country / 1 same continent / 3 other continent;
   * mults = CQ zones + countries (DXCC + WAE list — what cty.dat resolves)
   * per band; score = points × mults. */
  { "CQ WW", "CQ-WW",
    "[exchange]\ntx_serial=false\nfields=zone;\nzero_own_country=true\n"
    "points=" PTS_CQWW "\nmult=" MULT_CQWW "\n"
    "[field:zone]\nlabel=Zone\ntype=number\nadif_num=CQZ\nrequired=true\n",
    "CQ zone (OK = 15)" },
  /* cqwpx.com/rules: everyone works everyone, every QSO >= 1 point.
   * Scoring §V (2026-08-28): other continent 3 (6 on 40/80/160), same
   * continent 1 (2), own country 1 flat; mult = prefixes, once per
   * CONTEST, not per band. */
  { "CQ WPX", "CQ-WPX",
    "[exchange]\ntx_serial=true\nfields=nr;\n"
    "points=" PTS_WPX "\nmult=" MULT_WPX "\nmult_scope=contest\n"
    "[field:nr]\nlabel=Nr\ntype=serial\nrequired=true\n",
    NULL },
  /* IARU-HF-Rules.pdf (contests.arrl.org): "Any station may be contacted
   * on each band", points 1/3/5 — nothing scores zero. Scoring §5
   * (2026-08-28): HQ/officials (text exchange) 1 before the zone ladder,
   * same ITU zone 1, same continent 3, else 5; mults = zones + HQ
   * abbreviations per band (not mode) — the four officials AC/R1/R2/R3
   * cap themselves at four distinct values. Estimate: zones judged from
   * the received exchange (cty fallback). */
  { "IARU HF", "IARU-HF",
    "[exchange]\ntx_serial=false\nfields=exch;\n"
    "points=" PTS_IARU "\nmult=" MULT_IARU "\n"
    "[field:exch]\nlabel=Zone/HQ\ntype=auto\nadif_num=ITUZ\nrequired=true\n",
    "ITU zone (OK = 28)" },
  /* okomdx.crk.cz CW rules (2026): everyone works everyone — the old
   * OK/OM-with-foreigners-only restriction is gone; an OK station scores
   * even OK-OK (2 points). NB the CRC war statement: QSOs with RU/BY
   * stations are annulled — not modeled here (temporary, "until further
   * notice"). Scoring §6.1/§8/§9 (2026-08-28, OK seat): 2 own country /
   * 3 same continent (OM included) / 5 other; mults = OK/OM counties
   * (the received three-letter code) + countries (DXCC + WAE) per band —
   * §8 lists both types with no exclusion, so an OK/OM station brings its
   * county and its country. /MM's flat 5 points are not modeled. */
  { "OK/OM DX", "OK-OM-DX",
    "[exchange]\ntx_serial=false\nfields=exch;\n"
    "points=" PTS_OKOM "\nmult=" MULT_OKOM "\n"
    "[field:exch]\nlabel=Nr/District\ntype=auto\nrequired=true\n",
    "district (e.g. APA)" },
  /* Exchange is the two-digit year of the operator's FIRST licence
   * (euhf.s5cc.eu rules) — text, so "01" (= 2001) keeps its zero and the
   * multiplier string survives verbatim. Once per band and mode = exactly
   * the app's contest dup rule. Rules (PDF, updated 2026-06-30): "Only
   * continental Europe contacts count" — hence counts=eu-only. */
  { "EUHFC", "EU-HF",
    /* Scoring §7/§6/§8 (euhfc_rules_latest.pdf, 2026-08-28): 1 point per
     * valid QSO; mult = the received two-digit years, once per band
     * regardless of mode (QSO credit itself is per band AND mode — the
     * app's dup rule); score = points × mults. */
    "[exchange]\ntx_serial=false\nfields=year;\ncounts=eu-only\n"
    "points=" PTS_EUHFC "\nmult=" MULT_EUHFC "\n"
    "[field:year]\nlabel=Year\ntype=text\nrequired=true\n",
    "two-digit year of first licence (e.g. 99)" },
  /* WAE (darc.de rules): RST + progressive serial, "000" when the other
   * side sends none; "a contest QSO can only be conducted between a
   * European and a non-European station" — counts=eu-dx. The ADIF id
   * is per part — the CW prefill matches the August edition, SSB/RTTY
   * editions edit the suffix. Cabrillo CONTEST equals the ADIF id per the
   * WA7BNM master list; DARC's own rules name no value. DARC-WAEDC-CW
   * confirmed live: the DARC robot accepted OK1BR's 2026 log under it. */
  { "WAE DX", "DARC-WAEDC-CW",
    /* Scoring §5/§6/§8 (darc.de WAE rules, 2026-08-28): 1 point per QSO
     * (derived from §8 "total QSOs plus QTCs"); mults = worked entities
     * with numerical call areas split in W VE VK ZL ZS JA BY PY RA8/9/0,
     * weighted 80m×4 / 40m×3 / 20-10m×2. The eu-dx validity restricts an
     * EU seat's mults to non-EU; for the RTTY leg (no continental limit,
     * §12) the operator edits counts= and the mult rule then counts every
     * side's entities, which is what §12 asks. QTC points are NOT counted
     * (QTC traffic is a deferred SCOPE item) — the claimed score is a
     * documented underestimate until then. */
    "[exchange]\ntx_serial=true\nfields=nr;\ncounts=eu-dx\n"
    "points=" PTS_WAE "\nmult=" MULT_WAE "\nmult_weight=" WEIGHT_WAE "\n"
    "[field:nr]\nlabel=Nr\ntype=serial\nrequired=true\n",
    NULL },
  /* CVA (cvadx.org/regulamento): we send RS(T) + continent ("599 EU"); PY
   * stations answer with their state — both two-letter TEXT, never a
   * serial. Everyone works everyone (2/3/4 points by distance) — not a
   * PY-only contest despite the exchange shape. SSB edition edits the
   * ADIF id suffix. */
  { "CVA DX", "CVA-DX-CW",
    /* Scoring §9-§11 (cvadx.org English rules PDF, 2026-08-28): 2 own
     * country / 3 same continent / 4 other; mults = Brazilian states
     * (the exchange text, PY stations only — everyone else sends their
     * continent, which must not count) + countries per band. "Country"
     * is not pinned to DXCC by the rules; cty.dat is our reading. */
    "[exchange]\ntx_serial=false\nfields=cont;\n"
    "points=" PTS_CVA "\nmult=" MULT_CVA "\n"
    "[field:cont]\nlabel=Cont/State\ntype=text\nrequired=true\n",
    "continent (OK = EU)" },
  /* SARTG WW RTTY (sartg.com/contest/wwrules.htm): everyone works
   * everyone, 5/10/15 points; the rules themselves fix the Cabrillo
   * CONTEST name to SARTG-RTTY. Scoring (2026-08-28): 5 own country /
   * 10 same continent / 15 other. NO mult rule on purpose: the official
   * wording ("Each DXCC country ... including first contact with
   * Australia, Canada, Japan and USA" + call areas) leaves genuinely
   * ambiguous whether the DXCC entity counts on top of the call areas —
   * an ambiguity is not encoded, so the app shows points only. */
  { "SARTG WW RTTY", "SARTG-RTTY",
    "[exchange]\ntx_serial=true\nfields=nr;\n"
    "points=" PTS_SARTG "\n"
    "[field:nr]\nlabel=Nr\ntype=serial\nrequired=true\n",
    NULL },
  /* yodx.ro official rules ("Rules for the YO DX HF english.pdf", §5/§6.1):
   * we send RS(T) + serial from 001, YO stations answer with a county
   * abbreviation ("BU") — one AUTO field carries both, exactly like OK/OM
   * DX. Everyone works everyone and nothing scores zero from our seat
   * (8 pts YO / 4 other continent / 2 same continent / 1 same DXCC), hence
   * neither counts= nor zero_own_country. The YO-side rules that do zero
   * out (YO-YO = 0 pts, no county multipliers for YO stations, §6.2/§8.2)
   * are not modeled — they never apply to an OK entry. §7.1 dup = same
   * call+band+mode, which is the app's contest dup rule already. Cabrillo
   * CONTEST is YO-DX-HF (WA7BNM master list), mapped in win.c; YOHFDX is
   * the ADIF 3.1.5 Contest_ID value. */
  { "YO DX", "YOHFDX",
    /* Scoring §6.1/§8.1/§9 (yodx.ro rules, 2026-08-28, non-YO seat):
     * 8 YO station / 4 other continent / 2 same continent / 1 own DXCC —
     * term order matters, YO before own-country before the continents.
     * Mults = YO counties (the received text, YO stations only) + DXCC
     * countries, per band regardless of mode; §8.1 lists both types with
     * no exclusion, so a YO station brings its county and YO itself.
     * /MM's flat 4 points are not modeled. */
    "[exchange]\ntx_serial=true\nfields=exch;\n"
    "points=" PTS_YODX "\nmult=" MULT_YODX "\n"
    "[field:exch]\nlabel=Nr/County\ntype=auto\nrequired=true\n",
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
