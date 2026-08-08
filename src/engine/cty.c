/* cty.c — country/continent from a callsign prefix (see cty.h).
 *
 * cty.dat format (country-files.com/cty-dat-format): one record per entity.
 * The first line holds eight colon-terminated fields —
 *   Name: CQ zone: ITU zone: Continent: Lat: Long: GMT offset: Primary —
 * the primary prefix starting with '*' for WAE-only entities. Indented
 * continuation lines list comma-separated aliases until a ';'. An alias is
 * a prefix, or an exact call when it starts with '='; either may carry
 * overrides: "(cq)", "[itu]", "{continent}", "<lat/long>", "~tz~".
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "cty.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  char    *name;
  char    *prefix;             /* primary, '*' stripped */
  char     continent[3];
  int      cq_zone, itu_zone;
  gboolean waedc_only;
} CtyEntity;

typedef struct {
  CtyEntity *ent;
  char       continent[3];     /* effective values, overrides applied */
  int        cq_zone, itu_zone;
} CtyAlias;

struct _LogflCty {
  GPtrArray  *entities;        /* CtyEntity*, owned */
  GHashTable *prefixes;        /* g_strdup alias → CtyAlias*, both owned */
  GHashTable *exact;           /* "=CALL" entries, same value shape */
  char       *version;         /* "yyyymmdd" from =VER…, or NULL */
  guint       max_prefix_len;
};

static void
cty_entity_free (gpointer p)
{
  CtyEntity *e = p;
  g_free (e->name);
  g_free (e->prefix);
  g_free (e);
}

void
logfl_cty_free (LogflCty *c)
{
  if (!c)
    return;
  g_clear_pointer (&c->prefixes, g_hash_table_unref);
  g_clear_pointer (&c->exact, g_hash_table_unref);
  g_clear_pointer (&c->entities, g_ptr_array_unref);
  g_free (c->version);
  g_free (c);
}

/* --- parsing ------------------------------------------------------------- */

/* "Name:  15:  28:  EU: … :  OK:" → a fresh CtyEntity, or NULL. */
static CtyEntity *
parse_header (const char *line)
{
  char **f = g_strsplit (line, ":", -1);
  CtyEntity *e = NULL;
  if (g_strv_length (f) >= 8)
    {
      for (guint i = 0; f[i]; i++)
        g_strstrip (f[i]);
      const char *prim = f[7];
      gboolean wae = prim[0] == '*';
      if (wae)
        prim++;
      if (*f[0] && *f[3] && *prim && strlen (f[3]) == 2)
        {
          e = g_new0 (CtyEntity, 1);
          e->name = g_strdup (f[0]);
          e->cq_zone = atoi (f[1]);
          e->itu_zone = atoi (f[2]);
          memcpy (e->continent, f[3], 2);
          e->prefix = g_strdup (prim);
          e->waedc_only = wae;
        }
    }
  g_strfreev (f);
  return e;
}

/* One alias token, e.g. "OK", "=4U1WB(5)[8]", "VP8{SA}". Registers it into
 * the right table with the entity's values plus any overrides. */
static void
add_alias (LogflCty *c, CtyEntity *e, const char *tok)
{
  gboolean exact = tok[0] == '=';
  if (exact)
    tok++;
  if (!*tok)
    return;

  CtyAlias *a = g_new0 (CtyAlias, 1);
  a->ent = e;
  a->cq_zone = e->cq_zone;
  a->itu_zone = e->itu_zone;
  memcpy (a->continent, e->continent, 2);

  /* The bare alias ends at the first override bracket. */
  gsize n = strcspn (tok, "([{<~");
  char *key = g_ascii_strup (tok, (gssize) n);

  for (const char *p = tok + n; *p;)
    {
      const char *close;
      switch (*p)
        {
        case '(':
          a->cq_zone = atoi (p + 1);
          close = strchr (p, ')');
          break;
        case '[':
          a->itu_zone = atoi (p + 1);
          close = strchr (p, ']');
          break;
        case '{':
          if (strlen (p + 1) >= 2)
            memcpy (a->continent, p + 1, 2);
          close = strchr (p, '}');
          break;
        case '<':
          close = strchr (p, '>');
          break;
        case '~':
          close = strchr (p + 1, '~');
          break;
        default:
          close = NULL;
          break;
        }
      if (!close)
        break;
      p = close + 1;
    }

  if (exact && g_str_has_prefix (key, "VER") &&
      strlen (key) == 3 + 8 && !c->version)
    c->version = g_strdup (key + 3);

  GHashTable *table = exact ? c->exact : c->prefixes;
  if (!exact)
    c->max_prefix_len = MAX (c->max_prefix_len, (guint) strlen (key));
  g_hash_table_replace (table, key, a);   /* later duplicates win, as in cty */
}

LogflCty *
logfl_cty_load (const char *path, GError **error)
{
  char *data = NULL;
  if (!g_file_get_contents (path, &data, NULL, error))
    return NULL;

  LogflCty *c = g_new0 (LogflCty, 1);
  c->entities = g_ptr_array_new_with_free_func (cty_entity_free);
  c->prefixes = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       g_free, g_free);
  c->exact    = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       g_free, g_free);

  CtyEntity *cur = NULL;
  GString *aliases = g_string_new (NULL);
  char **lines = g_strsplit (data, "\n", -1);
  g_free (data);

  for (guint i = 0; lines[i]; i++)
    {
      const char *line = lines[i];
      if (!g_ascii_isspace (line[0]) && strchr (line, ':'))
        {
          CtyEntity *e = parse_header (line);
          if (e)
            {
              cur = e;
              g_ptr_array_add (c->entities, e);
              g_string_truncate (aliases, 0);
            }
          continue;
        }
      if (!cur)
        continue;
      g_string_append (aliases, line);
      char *end = strchr (aliases->str, ';');
      if (!end)
        continue;
      *end = '\0';
      char **tok = g_strsplit (aliases->str, ",", -1);
      for (guint t = 0; tok[t]; t++)
        add_alias (c, cur, g_strstrip (tok[t]));
      g_strfreev (tok);
      g_string_truncate (aliases, 0);
      cur = NULL;
    }
  g_strfreev (lines);
  g_string_free (aliases, TRUE);

  if (c->entities->len == 0)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                   "%s: no cty entities parsed", path);
      logfl_cty_free (c);
      return NULL;
    }
  return c;
}

char *
logfl_cty_locate (void)
{
  char *p = g_build_filename (g_get_user_data_dir (), "log-for-linux",
                              "cty.dat", NULL);
  if (g_file_test (p, G_FILE_TEST_IS_REGULAR))
    return p;
  g_free (p);

  const char *const *dirs = g_get_system_data_dirs ();
  for (guint i = 0; dirs[i]; i++)
    {
      p = g_build_filename (dirs[i], "log-for-linux", "cty.dat", NULL);
      if (g_file_test (p, G_FILE_TEST_IS_REGULAR))
        return p;
      g_free (p);
    }

  p = g_build_filename ("data", "cty.dat", NULL);   /* build-dir run */
  if (g_file_test (p, G_FILE_TEST_IS_REGULAR))
    return p;
  g_free (p);
  return NULL;
}

/* --- lookup -------------------------------------------------------------- */

/* Portable suffixes that keep the base call's country. Single letters and
 * digits ("OK1BR/3", "W1AW/7") are handled separately. */
static gboolean
suffix_is_transparent (const char *s)
{
  static const char *known[] = { "P", "M", "QRP", "LH", "A", "J", NULL };
  if (strlen (s) == 1 && (g_ascii_isdigit (s[0]) || g_ascii_isalpha (s[0])))
    return TRUE;
  for (guint i = 0; known[i]; i++)
    if (g_str_equal (s, known[i]))
      return TRUE;
  return FALSE;
}

/* The string whose prefix decides the country: base call normally, the
 * designator part for "DL/OK1BR" style portables. NULL = no country
 * (maritime/aeronautical mobile). Caller frees. */
static char *
match_candidate (const char *up)
{
  char **tok = g_strsplit (up, "/", -1);
  guint n = g_strv_length (tok);
  char *out = NULL;

  /* Trailing /MM, /AM: no fixed country by definition. */
  if (n >= 2 &&
      (g_str_equal (tok[n - 1], "MM") || g_str_equal (tok[n - 1], "AM")))
    {
      g_strfreev (tok);
      return NULL;
    }
  /* Drop transparent suffixes ("/P", "/7", "/QRP"), possibly stacked. */
  while (n >= 2 && suffix_is_transparent (tok[n - 1]))
    {
      g_free (tok[n - 1]);
      tok[--n] = NULL;
    }
  if (n == 1)
    out = g_strdup (tok[0]);
  else
    {
      /* "DL/OK1BR" or "OK1BR/3D2" — the shorter part is the designator;
       * a tie means a full call on both sides, take the first. */
      char *a = tok[0], *b = tok[n - 1];
      out = g_strdup (strlen (b) < strlen (a) ? b : a);
    }
  g_strfreev (tok);
  return out;
}

gboolean
logfl_cty_lookup (LogflCty *c, const char *call, LogflCtyInfo *out)
{
  g_return_val_if_fail (c != NULL, FALSE);
  memset (out, 0, sizeof *out);
  if (!call || !*call)
    return FALSE;

  char *up = g_ascii_strup (call, -1);
  g_strstrip (up);

  /* Exact entries match the call as heard, before any portable logic. */
  const CtyAlias *a = g_hash_table_lookup (c->exact, up);
  if (!a)
    {
      char *cand = match_candidate (up);
      if (cand)
        {
          for (gsize len = MIN (strlen (cand), c->max_prefix_len);
               len >= 1 && !a; len--)
            {
              char save = cand[len];
              cand[len] = '\0';
              a = g_hash_table_lookup (c->prefixes, cand);
              cand[len] = save;
            }
          g_free (cand);
        }
    }
  g_free (up);

  if (!a)
    return FALSE;
  out->country = a->ent->name;
  out->prefix = a->ent->prefix;
  memcpy (out->continent, a->continent, 2);
  out->cq_zone = a->cq_zone;
  out->itu_zone = a->itu_zone;
  out->waedc_only = a->ent->waedc_only;
  return TRUE;
}

const char *
logfl_cty_version (LogflCty *c)
{
  return c ? c->version : NULL;
}

guint
logfl_cty_n_entities (LogflCty *c)
{
  return c ? c->entities->len : 0;
}
