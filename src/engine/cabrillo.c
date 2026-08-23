/* cabrillo.c — Cabrillo v3 export (see cabrillo.h).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "cabrillo.h"
#include "engine.h"

#include <math.h>
#include <string.h>

/* Generic Cabrillo frequency per ADIF band: kHz band edge on HF (what the
 * EUHFC rules ask for when no exact QRG is logged), band designator on
 * VHF and up. */
static const struct {
  const char *band;
  const char *generic;         /* used when the QSO has no exact freq */
  gboolean designator;         /* VHF+: always use this, never kHz */
} band_freq[] = {
  { "2190m", "136", FALSE },  { "630m", "472", FALSE },
  { "160m", "1800", FALSE },  { "80m", "3500", FALSE },
  { "60m", "5300", FALSE },   { "40m", "7000", FALSE },
  { "30m", "10100", FALSE },  { "20m", "14000", FALSE },
  { "17m", "18068", FALSE },  { "15m", "21000", FALSE },
  { "12m", "24890", FALSE },  { "10m", "28000", FALSE },
  { "6m", "50", TRUE },       { "4m", "70", TRUE },
  { "2m", "144", TRUE },      { "1.25m", "222", TRUE },
  { "70cm", "432", TRUE },    { "33cm", "902", TRUE },
  { "23cm", "1.2G", TRUE },
};

/* Cabrillo freq column for one QSO. Caller frees. */
static char *
qso_freq_str (const LogflQso *q)
{
  const char *generic = "0";
  gboolean designator = FALSE;
  for (gsize i = 0; i < G_N_ELEMENTS (band_freq); i++)
    if (g_strcmp0 (band_freq[i].band, q->band) == 0)
      {
        generic = band_freq[i].generic;
        designator = band_freq[i].designator;
        break;
      }
  if (!designator && q->freq > 0)
    return g_strdup_printf ("%.0f", q->freq * 1000.0);
  return g_strdup (generic);
}

/* ADIF mode → Cabrillo mode column. */
static const char *
qso_mode_str (const LogflQso *q)
{
  if (g_strcmp0 (q->mode, "CW") == 0)
    return "CW";
  if (g_strcmp0 (q->mode, "SSB") == 0 || g_strcmp0 (q->mode, "AM") == 0)
    return "PH";
  if (g_strcmp0 (q->mode, "FM") == 0)
    return "FM";
  if (g_strcmp0 (q->mode, "RTTY") == 0)
    return "RY";
  return "DG";                 /* FT8, FT4, PSK31, … */
}

/* One side's exchange: serial (zero-padded when it is ours) + text. */
static char *
exch_str (gint64 serial, const char *text, gboolean pad_serial)
{
  GString *s = g_string_new (NULL);
  if (serial > 0)
    g_string_append_printf (s, pad_serial ? "%03" G_GINT64_FORMAT
                                          : "%" G_GINT64_FORMAT, serial);
  if (text && *text)
    {
      if (s->len)
        g_string_append_c (s, ' ');
      g_string_append (s, text);
    }
  if (!s->len)
    g_string_append_c (s, '-');
  return g_string_free (s, FALSE);
}

static const char *
rst_or_default (const char *rst, const char *mode_col)
{
  if (rst && *rst)
    return rst;
  return (g_str_equal (mode_col, "PH") || g_str_equal (mode_col, "FM"))
             ? "59" : "599";
}

static void
put_tag (GString *out, const char *tag, const char *val)
{
  if (val && *val)
    g_string_append_printf (out, "%s: %s\n", tag, val);
}

char *
logfl_cabrillo_export (LogflStore *s, gint64 contest_id,
                       const LogflCabrilloOpts *o,
                       guint *n_exported, GError **error)
{
  if (!o || !o->contest || !*o->contest || !o->callsign || !*o->callsign)
    {
      g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID,
                   "Cabrillo needs CONTEST and CALLSIGN");
      return NULL;
    }

  LogflStoreQuery query = { .contest = contest_id };
  GPtrArray *list = logfl_store_list (s, &query, error);
  if (!list)
    return NULL;

  GString *out = g_string_new ("START-OF-LOG: 3.0\n");
  put_tag (out, "CALLSIGN", o->callsign);
  put_tag (out, "CONTEST", o->contest);
  put_tag (out, "CATEGORY-OPERATOR", o->cat_operator);
  put_tag (out, "CATEGORY-BAND", o->cat_band);
  put_tag (out, "CATEGORY-POWER", o->cat_power);
  put_tag (out, "CATEGORY-MODE", o->cat_mode);
  put_tag (out, "CATEGORY-TRANSMITTER", o->cat_transmitter);
  put_tag (out, "CATEGORY-ASSISTED", o->cat_assisted);
  put_tag (out, "CLAIMED-SCORE", o->claimed_score);
  put_tag (out, "CLUB", o->club);
  put_tag (out, "OPERATORS", o->operators);
  put_tag (out, "NAME", o->name);
  put_tag (out, "EMAIL", o->email);
  put_tag (out, "LOCATION", o->location);
  put_tag (out, "GRID-LOCATOR", o->grid);
  char *created = g_strdup_printf ("log-for-linux %s",
                                   logfl_engine_version ());
  put_tag (out, "CREATED-BY", created);
  g_free (created);
  put_tag (out, "SOAPBOX", o->soapbox);

  /* list is newest-first; the spec wants chronological order. */
  guint n_lines = 0;
  for (guint i = list->len; i-- > 0; )
    {
      const LogflQso *q = list->pdata[i];
      GDateTime *dt = g_date_time_new_from_unix_utc (q->ts);
      if (!dt)
        continue;
      n_lines++;
      char *date = g_date_time_format (dt, "%Y-%m-%d");
      char *time = g_date_time_format (dt, "%H%M");
      g_date_time_unref (dt);

      char *freq = qso_freq_str (q);
      const char *mode = qso_mode_str (q);
      const char *mycall = q->station_callsign && *q->station_callsign
                               ? q->station_callsign : o->callsign;
      char *sent = exch_str (q->stx, q->stx_string, TRUE);
      char *rcvd = exch_str (q->srx, q->srx_string, FALSE);

      g_string_append_printf (out,
          "QSO: %5s %-2s %s %s %-13s %-3s %-6s %-13s %-3s %s\n",
          freq, mode, date, time,
          mycall, rst_or_default (q->rst_sent, mode), sent,
          q->call, rst_or_default (q->rst_rcvd, mode), rcvd);

      g_free (rcvd);
      g_free (sent);
      g_free (freq);
      g_free (time);
      g_free (date);
    }

  g_string_append (out, "END-OF-LOG:\n");
  if (n_exported)
    *n_exported = n_lines;
  g_ptr_array_unref (list);
  return g_string_free (out, FALSE);
}

gboolean
logfl_cabrillo_export_file (LogflStore *s, gint64 contest_id,
                            const char *path, const LogflCabrilloOpts *o,
                            guint *n_exported, GError **error)
{
  char *data = logfl_cabrillo_export (s, contest_id, o, n_exported, error);
  if (!data)
    return FALSE;
  gboolean ok = g_file_set_contents (path, data, -1, error);
  g_free (data);
  return ok;
}

/* --- categories derived from the log --------------------------------- */

/* ADIF mode -> CATEGORY-MODE family. Deliberately coarser than the QSO
 * line's mode column (which uses PH/RY/DG): the header takes the entrant
 * category names from the Cabrillo v3 tag list. */
static const char *
cat_mode_family (const char *adif_mode)
{
  if (!adif_mode || !*adif_mode)
    return NULL;
  if (g_ascii_strcasecmp (adif_mode, "CW") == 0)
    return "CW";
  if (g_ascii_strcasecmp (adif_mode, "SSB") == 0
      || g_ascii_strcasecmp (adif_mode, "AM") == 0)
    return "SSB";
  if (g_ascii_strcasecmp (adif_mode, "FM") == 0)
    return "FM";
  if (g_ascii_strcasecmp (adif_mode, "RTTY") == 0)
    return "RTTY";
  return "DIGI";               /* FT8, FT4, PSK31, MFSK, … */
}

/* ADIF band -> CATEGORY-BAND value. NULL = this build cannot name it, and
 * the caller must not guess (an unmapped band could belong to a category
 * the entrant would have to pick by hand anyway). */
static const char *
cat_band_value (const char *adif_band)
{
  static const struct { const char *adif, *cab; } map[] = {
    { "160m", "160M" }, { "80m", "80M" },   { "40m", "40M" },
    { "20m", "20M" },   { "15m", "15M" },   { "10m", "10M" },
    { "6m", "6M" },     { "4m", "4M" },     { "2m", "2M" },
    { "1.25m", "222" }, { "70cm", "432" },  { "33cm", "902" },
    { "23cm", "1.2G" },
  };
  if (!adif_band || !*adif_band)
    return NULL;
  for (gsize i = 0; i < G_N_ELEMENTS (map); i++)
    if (g_ascii_strcasecmp (map[i].adif, adif_band) == 0)
      return map[i].cab;
  return NULL;
}

gboolean
logfl_cabrillo_categories_from_log (LogflStore *s, gint64 contest_id,
                                    char **cat_mode, char **cat_band,
                                    GError **error)
{
  g_return_val_if_fail (s != NULL, FALSE);

  if (cat_mode)
    *cat_mode = NULL;
  if (cat_band)
    *cat_band = NULL;

  LogflStoreQuery query = { .contest = contest_id };
  GPtrArray *list = logfl_store_list (s, &query, error);
  if (!list)
    return FALSE;

  const char *mode = NULL;
  gboolean mode_mixed = FALSE, mode_seen = FALSE;
  const char *band = NULL;
  gboolean band_all = FALSE, band_seen = FALSE, band_unknown = FALSE;

  for (guint i = 0; i < list->len; i++)
    {
      const LogflQso *q = list->pdata[i];

      const char *m = cat_mode_family (q->mode);
      if (m)
        {
          if (!mode_seen)
            {
              mode = m;
              mode_seen = TRUE;
            }
          else if (g_strcmp0 (mode, m) != 0)
            mode_mixed = TRUE;
        }

      const char *b = cat_band_value (q->band);
      if (!b)
        band_unknown = TRUE;
      else if (!band_seen)
        {
          band = b;
          band_seen = TRUE;
        }
      else if (g_strcmp0 (band, b) != 0)
        band_all = TRUE;
    }

  if (cat_mode && mode_seen)
    *cat_mode = g_strdup (mode_mixed ? "MIXED" : mode);
  /* An unmapped band poisons the answer only while the log is otherwise
   * single-band: once two known bands disagree the entry is ALL either
   * way, and the unknown one cannot change that. */
  if (cat_band && band_seen && !(band_unknown && !band_all))
    *cat_band = g_strdup (band_all ? "ALL" : band);

  g_ptr_array_unref (list);
  return TRUE;
}
