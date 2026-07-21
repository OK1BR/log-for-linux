/* adif.c — ADIF 3.1.x import/export (see adif.h).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "adif.h"
#include "engine.h"

#include <string.h>
#include <stdlib.h>

/* --- band plan ---------------------------------------------------------- */

static const struct {
  double lo, hi;               /* MHz, inclusive */
  const char *band;
} band_table[] = {
  { 0.1357, 0.1378, "2190m" }, { 0.472, 0.479, "630m" },
  { 1.8, 2.0, "160m" },  { 3.5, 4.0, "80m" },   { 5.06, 5.45, "60m" },
  { 7.0, 7.3, "40m" },   { 10.1, 10.15, "30m" },{ 14.0, 14.35, "20m" },
  { 18.068, 18.168, "17m" }, { 21.0, 21.45, "15m" },
  { 24.89, 24.99, "12m" },   { 28.0, 29.7, "10m" },
  { 50.0, 54.0, "6m" },  { 70.0, 71.0, "4m" },  { 144.0, 148.0, "2m" },
  { 222.0, 225.0, "1.25m" }, { 420.0, 450.0, "70cm" },
  { 902.0, 928.0, "33cm" },  { 1240.0, 1300.0, "23cm" },
};

const char *
logfl_adif_band_for_freq (double mhz)
{
  for (gsize i = 0; i < G_N_ELEMENTS (band_table); i++)
    if (mhz >= band_table[i].lo && mhz <= band_table[i].hi)
      return band_table[i].band;
  return NULL;
}

double
logfl_adif_freq_for_band (const char *band)
{
  if (!band || !*band)
    return 0;
  for (gsize i = 0; i < G_N_ELEMENTS (band_table); i++)
    if (g_ascii_strcasecmp (band_table[i].band, band) == 0)
      return (band_table[i].lo + band_table[i].hi) / 2.0;
  return 0;
}

/* --- parser ------------------------------------------------------------- */

/* One length-prefixed tag: <NAME:len[:type]>value, or the bare <EOR>/<EOH>
 * tokens. Returns FALSE when no further tag exists. */
typedef struct {
  char     name[64];           /* uppercased */
  char    *value;              /* g_strndup'd, NULL for EOR/EOH */
  gboolean truncated;          /* declared length ran past EOF */
} AdifTag;

static gboolean
next_tag (const char **pp, const char *end, AdifTag *tag)
{
  const char *p = *pp;

  for (;;)
    {
      while (p < end && *p != '<')
        p++;
      if (p >= end)
        return FALSE;
      const char *gt = memchr (p + 1, '>', (gsize) (end - p - 1));
      if (!gt)
        return FALSE;

      /* Split "NAME[:len[:type]]". */
      gsize inlen = (gsize) (gt - p - 1);
      if (inlen == 0 || inlen >= sizeof tag->name)
        {
          p++;                 /* not a plausible tag — scan on */
          continue;
        }
      char inner[64];
      memcpy (inner, p + 1, inlen);
      inner[inlen] = '\0';

      char *colon = strchr (inner, ':');
      if (!colon)
        {
          if (g_ascii_strcasecmp (inner, "EOR") != 0 &&
              g_ascii_strcasecmp (inner, "EOH") != 0)
            {
              p++;             /* "<something>" free text — skip */
              continue;
            }
          g_strlcpy (tag->name, inner, sizeof tag->name);
          for (char *c = tag->name; *c; c++)
            *c = g_ascii_toupper (*c);
          tag->value = NULL;
          tag->truncated = FALSE;
          *pp = gt + 1;
          return TRUE;
        }

      *colon = '\0';
      char *lenstr = colon + 1;
      char *colon2 = strchr (lenstr, ':');
      if (colon2)
        *colon2 = '\0';        /* drop the optional :type */
      char *endptr = NULL;
      long len = strtol (lenstr, &endptr, 10);
      if (endptr == lenstr || *endptr != '\0' || len < 0)
        {
          p++;                 /* "<no:tag>" garbage — skip */
          continue;
        }

      g_strlcpy (tag->name, inner, sizeof tag->name);
      for (char *c = tag->name; *c; c++)
        *c = g_ascii_toupper (*c);
      const char *val = gt + 1;
      gsize avail = (gsize) (end - val);
      tag->truncated = (gsize) len > avail;
      gsize take = tag->truncated ? avail : (gsize) len;
      tag->value = g_strndup (val, take);
      *pp = val + take;
      return TRUE;
    }
}

/* Pending record state while walking a file. */
typedef struct {
  LogflQso *q;
  GString  *extras;
  char      date[16], time[8];
  gboolean  any;               /* saw at least one field */
  gboolean  broken;            /* truncated tag inside this record */
} RecBuild;

static void
rec_reset (RecBuild *r)
{
  logfl_qso_free (r->q);
  r->q = logfl_qso_new ();
  g_string_truncate (r->extras, 0);
  r->date[0] = r->time[0] = '\0';
  r->any = FALSE;
  r->broken = FALSE;
}

static void
set_str (char **slot, const char *val)
{
  g_free (*slot);
  *slot = g_strdup (val);
}

static void
rec_field (RecBuild *r, const AdifTag *tag)
{
  LogflQso *q = r->q;
  const char *v = tag->value;

  r->any = TRUE;
  if (tag->truncated)
    r->broken = TRUE;

  if (g_str_equal (tag->name, "CALL"))
    set_str (&q->call, v);
  else if (g_str_equal (tag->name, "QSO_DATE"))
    g_strlcpy (r->date, v, sizeof r->date);
  else if (g_str_equal (tag->name, "TIME_ON"))
    g_strlcpy (r->time, v, sizeof r->time);
  else if (g_str_equal (tag->name, "BAND"))
    set_str (&q->band, v);
  else if (g_str_equal (tag->name, "FREQ"))
    q->freq = g_ascii_strtod (v, NULL);
  else if (g_str_equal (tag->name, "MODE"))
    set_str (&q->mode, v);
  else if (g_str_equal (tag->name, "SUBMODE"))
    set_str (&q->submode, v);
  else if (g_str_equal (tag->name, "RST_SENT"))
    set_str (&q->rst_sent, v);
  else if (g_str_equal (tag->name, "RST_RCVD"))
    set_str (&q->rst_rcvd, v);
  else if (g_str_equal (tag->name, "GRIDSQUARE"))
    set_str (&q->gridsquare, v);
  else if (g_str_equal (tag->name, "NAME"))
    set_str (&q->name, v);
  else if (g_str_equal (tag->name, "QTH"))
    set_str (&q->qth, v);
  else if (g_str_equal (tag->name, "TX_PWR"))
    q->tx_pwr = g_ascii_strtod (v, NULL);
  else if (g_str_equal (tag->name, "COMMENT"))
    set_str (&q->comment, v);
  else if (g_str_equal (tag->name, "QSL_RCVD"))
    set_str (&q->qsl_rcvd, v);
  else if (g_str_equal (tag->name, "QSL_SENT"))
    set_str (&q->qsl_sent, v);
  else if (g_str_equal (tag->name, "LOTW_QSL_RCVD"))
    set_str (&q->lotw_qsl_rcvd, v);
  else if (g_str_equal (tag->name, "LOTW_QSL_SENT"))
    set_str (&q->lotw_qsl_sent, v);
  else if (g_str_equal (tag->name, "EQSL_QSL_RCVD"))
    set_str (&q->eqsl_qsl_rcvd, v);
  else if (g_str_equal (tag->name, "EQSL_QSL_SENT"))
    set_str (&q->eqsl_qsl_sent, v);
  else if (g_str_equal (tag->name, "STATION_CALLSIGN"))
    set_str (&q->station_callsign, v);
  else if (g_str_equal (tag->name, "MY_GRIDSQUARE"))
    set_str (&q->my_gridsquare, v);
  else
    /* Unmodeled field — preserve verbatim (normalized to our own shape,
     * uppercase name and byte length, which is what export writes). */
    g_string_append_printf (r->extras, "<%s:%zu>%s",
                            tag->name, strlen (v), v);
}

/* QSO_DATE "YYYYMMDD" + TIME_ON "HHMM"|"HHMMSS" → unix UTC, 0 on nonsense. */
static gint64
parse_ts (const char *date, const char *time)
{
  if (strlen (date) != 8)
    return 0;
  for (const char *c = date; *c; c++)
    if (!g_ascii_isdigit (*c))
      return 0;
  gsize tlen = strlen (time);
  if (tlen != 0 && tlen != 4 && tlen != 6)
    return 0;
  for (const char *c = time; *c; c++)
    if (!g_ascii_isdigit (*c))
      return 0;

  int y = atoi ((char[]){ date[0], date[1], date[2], date[3], 0 });
  int mo = atoi ((char[]){ date[4], date[5], 0 });
  int d = atoi ((char[]){ date[6], date[7], 0 });
  int h = 0, mi = 0, se = 0;
  if (tlen >= 4)
    {
      h = atoi ((char[]){ time[0], time[1], 0 });
      mi = atoi ((char[]){ time[2], time[3], 0 });
    }
  if (tlen == 6)
    se = atoi ((char[]){ time[4], time[5], 0 });

  GDateTime *dt = g_date_time_new_utc (y, mo, d, h, mi, se);
  if (!dt)
    return 0;
  gint64 ts = g_date_time_to_unix (dt);
  g_date_time_unref (dt);
  return ts;
}

/* Closes a pending record into the store. */
static gboolean
rec_commit (LogflStore *s, RecBuild *r, guint dup_window_s,
            LogflAdifReport *report, GError **error)
{
  LogflQso *q = r->q;
  gboolean ok = TRUE;

  if (!r->any)
    return TRUE;               /* stray <EOR> */

  q->ts = parse_ts (r->date, r->time);
  if (!q->band && q->freq > 0)
    q->band = g_strdup (logfl_adif_band_for_freq (q->freq));
  if (r->extras->len)
    set_str (&q->extras, r->extras->str);

  if (r->broken || !q->call || !*q->call || q->ts == 0 ||
      !q->mode || !*q->mode || !q->band || !*q->band)
    {
      report->n_bad++;
      rec_reset (r);
      return TRUE;
    }

  gboolean dup = FALSE;
  GError *err = NULL;
  if (!logfl_store_dup_check (s, q->call, q->band, q->mode, q->ts,
                              dup_window_s, &dup, &err))
    ok = FALSE;
  else if (dup)
    report->n_dup_skipped++;
  else if (logfl_store_add (s, q, &err))
    report->n_imported++;
  else if (g_error_matches (err, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID))
    {
      report->n_bad++;         /* e.g. whitespace-only call */
      g_clear_error (&err);
    }
  else
    ok = FALSE;

  if (!ok)
    g_propagate_error (error, err);
  rec_reset (r);
  return ok;
}

gboolean
logfl_adif_import_data (LogflStore *s, const char *data, gssize len,
                        guint dup_window_s, LogflAdifReport *report,
                        GError **error)
{
  LogflAdifReport rep = { 0 };
  if (len < 0)
    len = (gssize) strlen (data);
  const char *p = data, *end = data + len;

  /* Header iff the first non-whitespace char is not '<' (ADIF rule); a
   * stray <EOH> in a headerless file just resets the pending record. */
  const char *first = p;
  while (first < end && g_ascii_isspace (*first))
    first++;
  gboolean in_header = first < end && *first != '<';

  RecBuild r = { .q = logfl_qso_new (), .extras = g_string_new (NULL) };
  gboolean ok = logfl_store_tx_begin (s, error);
  AdifTag tag;

  while (ok && next_tag (&p, end, &tag))
    {
      if (!tag.value)          /* <EOR> or <EOH> */
        {
          if (g_str_equal (tag.name, "EOH"))
            {
              in_header = FALSE;
              rec_reset (&r);  /* drop header fields */
            }
          else if (!in_header)
            ok = rec_commit (s, &r, dup_window_s, &rep, error);
          continue;
        }
      if (!in_header)
        rec_field (&r, &tag);
      g_free (tag.value);
    }
  /* Tolerate a final record with no <EOR>. */
  if (ok && !in_header)
    ok = rec_commit (s, &r, dup_window_s, &rep, error);

  if (ok)
    ok = logfl_store_tx_commit (s, error);
  else
    logfl_store_tx_rollback (s, NULL);

  logfl_qso_free (r.q);
  g_string_free (r.extras, TRUE);
  if (report)
    *report = rep;
  return ok;
}

gboolean
logfl_adif_import_file (LogflStore *s, const char *path, guint dup_window_s,
                        LogflAdifReport *report, GError **error)
{
  char *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents (path, &data, &len, error))
    return FALSE;
  gboolean ok = logfl_adif_import_data (s, data, (gssize) len,
                                        dup_window_s, report, error);
  g_free (data);
  return ok;
}

/* --- writer ------------------------------------------------------------- */

static void
put_str (GString *out, const char *tag, const char *val)
{
  if (val && *val)
    g_string_append_printf (out, "<%s:%zu>%s", tag, strlen (val), val);
}

/* Locale-safe decimal with trailing zeros trimmed ("7.012300" → "7.0123"). */
static void
put_num (GString *out, const char *tag, double v)
{
  if (v <= 0)
    return;
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (buf, sizeof buf, "%.6f", v);
  char *dot = strchr (buf, '.');
  if (dot)
    {
      char *last = buf + strlen (buf) - 1;
      while (last > dot && *last == '0')
        *last-- = '\0';
      if (last == dot)
        *last = '\0';
    }
  put_str (out, tag, buf);
}

char *
logfl_adif_export_data (LogflStore *s, const LogflStoreQuery *query,
                        guint *n_exported, GError **error)
{
  GPtrArray *list = logfl_store_list (s, query, error);
  if (!list)
    return NULL;

  GString *out = g_string_new ("log-for-linux ADIF export\n");
  put_str (out, "ADIF_VER", "3.1.4");
  put_str (out, "PROGRAMID", "log-for-linux");
  put_str (out, "PROGRAMVERSION", logfl_engine_version ());
  g_string_append (out, "<EOH>\n");

  /* list is newest-first; write oldest-first. */
  for (guint i = list->len; i-- > 0; )
    {
      const LogflQso *q = list->pdata[i];
      GDateTime *dt = g_date_time_new_from_unix_utc (q->ts);
      if (dt)
        {
          char *date = g_date_time_format (dt, "%Y%m%d");
          char *time = g_date_time_format (dt, "%H%M%S");
          put_str (out, "QSO_DATE", date);
          put_str (out, "TIME_ON", time);
          g_free (date);
          g_free (time);
          g_date_time_unref (dt);
        }
      put_str (out, "CALL", q->call);
      put_str (out, "BAND", q->band);
      put_num (out, "FREQ", q->freq);
      put_str (out, "MODE", q->mode);
      put_str (out, "SUBMODE", q->submode);
      put_str (out, "RST_SENT", q->rst_sent);
      put_str (out, "RST_RCVD", q->rst_rcvd);
      put_str (out, "GRIDSQUARE", q->gridsquare);
      put_str (out, "NAME", q->name);
      put_str (out, "QTH", q->qth);
      put_num (out, "TX_PWR", q->tx_pwr);
      put_str (out, "COMMENT", q->comment);
      put_str (out, "QSL_RCVD", q->qsl_rcvd);
      put_str (out, "QSL_SENT", q->qsl_sent);
      put_str (out, "LOTW_QSL_RCVD", q->lotw_qsl_rcvd);
      put_str (out, "LOTW_QSL_SENT", q->lotw_qsl_sent);
      put_str (out, "EQSL_QSL_RCVD", q->eqsl_qsl_rcvd);
      put_str (out, "EQSL_QSL_SENT", q->eqsl_qsl_sent);
      put_str (out, "STATION_CALLSIGN", q->station_callsign);
      put_str (out, "MY_GRIDSQUARE", q->my_gridsquare);
      if (q->extras && *q->extras)
        g_string_append (out, q->extras);
      g_string_append (out, "<EOR>\n");
    }

  if (n_exported)
    *n_exported = list->len;
  g_ptr_array_unref (list);
  return g_string_free (out, FALSE);
}

gboolean
logfl_adif_export_file (LogflStore *s, const char *path,
                        const LogflStoreQuery *query,
                        guint *n_exported, GError **error)
{
  char *data = logfl_adif_export_data (s, query, n_exported, error);
  if (!data)
    return FALSE;
  gboolean ok = g_file_set_contents (path, data, -1, error);
  g_free (data);
  return ok;
}
