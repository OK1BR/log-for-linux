/* wsjtx_udp.c — WSJT-X UDP parse, pack, and listen server (M6).
 *
 * Protocol: NetworkMessage.hpp (WSJT-X). Magic 0xadbccbda, big-endian
 * QDataStream schema 2/3, utf8 = QByteArray.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "wsjtx_udp.h"

#include "adif.h"

#include <gio/gio.h>
#include <string.h>
#include <sys/socket.h>

G_DEFINE_QUARK (logfl-wsjtx-error-quark, logfl_wsjtx_error)

/* --- cursor over a datagram -------------------------------------------- */

typedef struct {
  const guint8 *p;
  gsize         n;
  gsize         i;
} Cur;

static gboolean
cur_need (const Cur *c, gsize n, GError **error)
{
  if (c->i + n > c->n)
    {
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_PARSE,
                   "WSJT-X message truncated");
      return FALSE;
    }
  return TRUE;
}

static gboolean
rd_u8 (Cur *c, guint8 *o, GError **error)
{
  if (!cur_need (c, 1, error))
    return FALSE;
  *o = c->p[c->i++];
  return TRUE;
}

static gboolean
rd_u32 (Cur *c, guint32 *o, GError **error)
{
  if (!cur_need (c, 4, error))
    return FALSE;
  *o = ((guint32) c->p[c->i] << 24) | ((guint32) c->p[c->i + 1] << 16) |
       ((guint32) c->p[c->i + 2] << 8) | (guint32) c->p[c->i + 3];
  c->i += 4;
  return TRUE;
}

static gboolean
rd_i32 (Cur *c, gint32 *o, GError **error)
{
  guint32 u;
  if (!rd_u32 (c, &u, error))
    return FALSE;
  *o = (gint32) u;
  return TRUE;
}

static gboolean
rd_u64 (Cur *c, guint64 *o, GError **error)
{
  if (!cur_need (c, 8, error))
    return FALSE;
  *o = ((guint64) c->p[c->i] << 56) | ((guint64) c->p[c->i + 1] << 48) |
       ((guint64) c->p[c->i + 2] << 40) | ((guint64) c->p[c->i + 3] << 32) |
       ((guint64) c->p[c->i + 4] << 24) | ((guint64) c->p[c->i + 5] << 16) |
       ((guint64) c->p[c->i + 6] << 8) | (guint64) c->p[c->i + 7];
  c->i += 8;
  return TRUE;
}

static gboolean
rd_i64 (Cur *c, gint64 *o, GError **error)
{
  guint64 u;
  if (!rd_u64 (c, &u, error))
    return FALSE;
  *o = (gint64) u;
  return TRUE;
}

/* utf8 / QByteArray: length quint32; 0xffffffff = null; 0 = empty. */
static gboolean
rd_utf8 (Cur *c, char **out, GError **error)
{
  guint32 n;
  if (!rd_u32 (c, &n, error))
    return FALSE;
  if (n == 0xffffffffU)
    {
      *out = NULL;
      return TRUE;
    }
  if (n > 65536)
    {
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_PARSE,
                   "WSJT-X string too long (%u)", n);
      return FALSE;
    }
  if (!cur_need (c, n, error))
    return FALSE;
  *out = g_strndup ((const char *) (c->p + c->i), n);
  c->i += n;
  return TRUE;
}

/* Optional utf8: on truncate leave *out NULL and return TRUE (older schema). */
static gboolean
rd_utf8_opt (Cur *c, char **out)
{
  if (c->i + 4 > c->n)
    {
      *out = NULL;
      return TRUE;
    }
  GError *err = NULL;
  if (!rd_utf8 (c, out, &err))
    {
      g_clear_error (&err);
      *out = NULL;
      /* rewind? already advanced partially — mark exhausted */
      c->i = c->n;
      return TRUE;
    }
  return TRUE;
}

/* QDateTime → unix UTC seconds. timespec 0=local (treated as UTC here —
 * WSJT-X sends UTC=1 for logged QSOs), 1=UTC, 2=offset (qint32 seconds). */
static gboolean
rd_qdatetime (Cur *c, gint64 *unix_s, GError **error)
{
  gint64 jd;
  guint32 ms;
  guint8 spec;
  if (!rd_i64 (c, &jd, error) || !rd_u32 (c, &ms, error) ||
      !rd_u8 (c, &spec, error))
    return FALSE;

  gint32 offset = 0;
  if (spec == 2)
    {
      if (!rd_i32 (c, &offset, error))
        return FALSE;
    }
  else if (spec == 3)
    {
      /* timezone object — skip remaining best-effort: treat as invalid */
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_PARSE,
                   "WSJT-X QDateTime timezone timespec not supported");
      return FALSE;
    }

  if (jd <= 0)
    {
      *unix_s = 0;
      return TRUE;
    }

  /* Julian day → civil date (Fliegel & Van Flandern / Qt fromJulianDay). */
  gint64 a = jd + 32044;
  gint64 b = (4 * a + 3) / 146097;
  gint64 c_ = a - (146097 * b) / 4;
  gint64 d = (4 * c_ + 3) / 1461;
  gint64 e = c_ - (1461 * d) / 4;
  gint64 m = (5 * e + 2) / 153;
  int day = (int) (e - (153 * m + 2) / 5 + 1);
  int month = (int) (m + 3 - 12 * (m / 10));
  int year = (int) (100 * b + d - 4800 + m / 10);

  GDateTime *dt = g_date_time_new_utc (year, month, day, 0, 0, 0);
  if (!dt)
    {
      *unix_s = 0;
      return TRUE;
    }
  gint64 base = g_date_time_to_unix (dt);
  g_date_time_unref (dt);
  *unix_s = base + (gint64) (ms / 1000) - (gint64) offset;
  return TRUE;
}

void
logfl_wsjtx_header_clear (LogflWsjtxHeader *h)
{
  if (!h)
    return;
  g_clear_pointer (&h->id, g_free);
  memset (h, 0, sizeof *h);
}

void
logfl_wsjtx_status_clear (LogflWsjtxStatus *s)
{
  if (!s)
    return;
  logfl_wsjtx_header_clear (&s->hdr);
  g_clear_pointer (&s->mode, g_free);
  g_clear_pointer (&s->dx_call, g_free);
  g_clear_pointer (&s->dx_grid, g_free);
  g_clear_pointer (&s->de_call, g_free);
  g_clear_pointer (&s->de_grid, g_free);
  g_clear_pointer (&s->report, g_free);
  g_clear_pointer (&s->tx_mode, g_free);
  memset (s, 0, sizeof *s);
}

void
logfl_wsjtx_qso_logged_clear (LogflWsjtxQsoLogged *q)
{
  if (!q)
    return;
  logfl_wsjtx_header_clear (&q->hdr);
  g_clear_pointer (&q->dx_call, g_free);
  g_clear_pointer (&q->dx_grid, g_free);
  g_clear_pointer (&q->mode, g_free);
  g_clear_pointer (&q->rst_sent, g_free);
  g_clear_pointer (&q->rst_rcvd, g_free);
  g_clear_pointer (&q->tx_power, g_free);
  g_clear_pointer (&q->comment, g_free);
  g_clear_pointer (&q->name, g_free);
  g_clear_pointer (&q->op_call, g_free);
  g_clear_pointer (&q->my_call, g_free);
  g_clear_pointer (&q->my_grid, g_free);
  g_clear_pointer (&q->exch_sent, g_free);
  g_clear_pointer (&q->exch_rcvd, g_free);
  g_clear_pointer (&q->prop_mode, g_free);
  memset (q, 0, sizeof *q);
}

gboolean
logfl_wsjtx_parse_header (const guint8 *data, gsize len, LogflWsjtxHeader *out,
                          gsize *payload_off, GError **error)
{
  g_return_val_if_fail (data != NULL && out != NULL, FALSE);
  memset (out, 0, sizeof *out);
  Cur c = { .p = data, .n = len, .i = 0 };
  guint32 magic = 0, schema = 0, type = 0;
  if (!rd_u32 (&c, &magic, error) || !rd_u32 (&c, &schema, error) ||
      !rd_u32 (&c, &type, error))
    return FALSE;
  if (magic != LOGFL_WSJTX_MAGIC)
    {
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_PARSE,
                   "bad WSJT-X magic 0x%08x", magic);
      return FALSE;
    }
  if (schema < 2 || schema > 3)
    {
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_PARSE,
                   "unsupported WSJT-X schema %u", schema);
      return FALSE;
    }
  char *id = NULL;
  if (!rd_utf8 (&c, &id, error))
    return FALSE;
  out->magic = magic;
  out->schema = schema;
  out->type = (LogflWsjtxMsgType) type;
  out->id = id;
  if (payload_off)
    *payload_off = c.i;
  return TRUE;
}

gboolean
logfl_wsjtx_parse_qso_logged (const guint8 *data, gsize len,
                              LogflWsjtxQsoLogged *out, GError **error)
{
  g_return_val_if_fail (out != NULL, FALSE);
  memset (out, 0, sizeof *out);
  gsize off = 0;
  if (!logfl_wsjtx_parse_header (data, len, &out->hdr, &off, error))
    return FALSE;
  if (out->hdr.type != LOGFL_WSJTX_MSG_QSO_LOGGED)
    {
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_PARSE,
                   "expected QSO Logged (5), got %u", (guint) out->hdr.type);
      logfl_wsjtx_qso_logged_clear (out);
      return FALSE;
    }
  Cur c = { .p = data, .n = len, .i = off };
  if (!rd_qdatetime (&c, &out->ts_off, error) ||
      !rd_utf8 (&c, &out->dx_call, error) ||
      !rd_utf8 (&c, &out->dx_grid, error) ||
      !rd_u64 (&c, &out->freq_hz, error) || !rd_utf8 (&c, &out->mode, error) ||
      !rd_utf8 (&c, &out->rst_sent, error) ||
      !rd_utf8 (&c, &out->rst_rcvd, error) ||
      !rd_utf8 (&c, &out->tx_power, error) ||
      !rd_utf8 (&c, &out->comment, error) || !rd_utf8 (&c, &out->name, error) ||
      !rd_qdatetime (&c, &out->ts_on, error) ||
      !rd_utf8 (&c, &out->op_call, error) ||
      !rd_utf8 (&c, &out->my_call, error) ||
      !rd_utf8 (&c, &out->my_grid, error))
    {
      logfl_wsjtx_qso_logged_clear (out);
      return FALSE;
    }
  rd_utf8_opt (&c, &out->exch_sent);
  rd_utf8_opt (&c, &out->exch_rcvd);
  rd_utf8_opt (&c, &out->prop_mode);
  return TRUE;
}

gboolean
logfl_wsjtx_parse_status (const guint8 *data, gsize len, LogflWsjtxStatus *out,
                          GError **error)
{
  g_return_val_if_fail (out != NULL, FALSE);
  memset (out, 0, sizeof *out);
  gsize off = 0;
  if (!logfl_wsjtx_parse_header (data, len, &out->hdr, &off, error))
    return FALSE;
  if (out->hdr.type != LOGFL_WSJTX_MSG_STATUS)
    {
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_PARSE,
                   "expected Status (1), got %u", (guint) out->hdr.type);
      logfl_wsjtx_status_clear (out);
      return FALSE;
    }
  Cur c = { .p = data, .n = len, .i = off };
  guint8 b;
  gint32 i32;
  /* dial, mode, dx, report, tx_mode, tx_en, txing, decoding, rx_df, tx_df,
   * de_call, de_grid, dx_grid — then optional tail ignored. */
  if (!rd_u64 (&c, &out->dial_hz, error) || !rd_utf8 (&c, &out->mode, error) ||
      !rd_utf8 (&c, &out->dx_call, error) ||
      !rd_utf8 (&c, &out->report, error) ||
      !rd_utf8 (&c, &out->tx_mode, error) || !rd_u8 (&c, &b, error) || /* tx_en */
      !rd_u8 (&c, &b, error) ||                                       /* txing */
      !rd_u8 (&c, &b, error) ||                                       /* dec */
      !rd_i32 (&c, &i32, error) || !rd_i32 (&c, &i32, error) ||
      !rd_utf8 (&c, &out->de_call, error) ||
      !rd_utf8 (&c, &out->de_grid, error) ||
      !rd_utf8 (&c, &out->dx_grid, error))
    {
      logfl_wsjtx_status_clear (out);
      return FALSE;
    }
  return TRUE;
}

static double
parse_power_w (const char *s)
{
  if (!s || !*s)
    return 0;
  char *end = NULL;
  double v = g_ascii_strtod (s, &end);
  if (end == s || v <= 0)
    return 0;
  return v;
}

LogflQso *
logfl_wsjtx_to_qso (const LogflWsjtxQsoLogged *m)
{
  g_return_val_if_fail (m != NULL, NULL);
  if (!m->dx_call || !m->dx_call[0])
    return NULL;

  LogflQso *q = logfl_qso_new ();
  q->ts = m->ts_on > 0 ? m->ts_on : (m->ts_off > 0 ? m->ts_off : 0);
  q->call = g_strdup (m->dx_call);
  if (m->freq_hz > 0)
    {
      q->freq = (double) m->freq_hz / 1e6;
      const char *band = logfl_adif_band_for_freq (q->freq);
      if (band)
        q->band = g_strdup (band);
    }
  if (m->mode && m->mode[0])
    q->mode = g_strdup (m->mode);
  if (m->rst_sent && m->rst_sent[0])
    q->rst_sent = g_strdup (m->rst_sent);
  if (m->rst_rcvd && m->rst_rcvd[0])
    q->rst_rcvd = g_strdup (m->rst_rcvd);
  if (m->dx_grid && m->dx_grid[0])
    q->gridsquare = g_strdup (m->dx_grid);
  if (m->name && m->name[0])
    q->name = g_strdup (m->name);
  if (m->comment && m->comment[0])
    q->comment = g_strdup (m->comment);
  q->tx_pwr = parse_power_w (m->tx_power);
  if (m->my_call && m->my_call[0])
    q->station_callsign = g_strdup (m->my_call);
  if (m->my_grid && m->my_grid[0])
    q->my_gridsquare = g_strdup (m->my_grid);

  /* Preserve contest exchange / prop in extras as ADIF-ish tags for M2. */
  if ((m->exch_sent && m->exch_sent[0]) || (m->exch_rcvd && m->exch_rcvd[0]) ||
      (m->prop_mode && m->prop_mode[0]) || (m->op_call && m->op_call[0]))
    {
      GString *ex = g_string_new (NULL);
      if (m->op_call && m->op_call[0])
        g_string_append_printf (ex, "<OPERATOR:%u>%s",
                                (guint) strlen (m->op_call), m->op_call);
      if (m->exch_sent && m->exch_sent[0])
        g_string_append_printf (ex, "<STX_STRING:%u>%s",
                                (guint) strlen (m->exch_sent), m->exch_sent);
      if (m->exch_rcvd && m->exch_rcvd[0])
        g_string_append_printf (ex, "<SRX_STRING:%u>%s",
                                (guint) strlen (m->exch_rcvd), m->exch_rcvd);
      if (m->prop_mode && m->prop_mode[0])
        g_string_append_printf (ex, "<PROP_MODE:%u>%s",
                                (guint) strlen (m->prop_mode), m->prop_mode);
      q->extras = g_string_free (ex, FALSE);
    }
  return q;
}

/* --- writers ----------------------------------------------------------- */

static void
wr_u8 (GByteArray *a, guint8 v)
{
  g_byte_array_append (a, &v, 1);
}

static void
wr_u32 (GByteArray *a, guint32 v)
{
  guint8 b[4] = { (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff,
                  v & 0xff };
  g_byte_array_append (a, b, 4);
}

static void
wr_i32 (GByteArray *a, gint32 v)
{
  wr_u32 (a, (guint32) v);
}

static void
wr_u64 (GByteArray *a, guint64 v)
{
  guint8 b[8] = { (v >> 56) & 0xff, (v >> 48) & 0xff, (v >> 40) & 0xff,
                  (v >> 32) & 0xff, (v >> 24) & 0xff, (v >> 16) & 0xff,
                  (v >> 8) & 0xff,  v & 0xff };
  g_byte_array_append (a, b, 8);
}

static void
wr_i64 (GByteArray *a, gint64 v)
{
  wr_u64 (a, (guint64) v);
}

static void
wr_utf8 (GByteArray *a, const char *s)
{
  if (!s)
    {
      wr_u32 (a, 0xffffffffU);
      return;
    }
  gsize n = strlen (s);
  wr_u32 (a, (guint32) n);
  if (n)
    g_byte_array_append (a, (const guint8 *) s, (guint) n);
}

/* QDateTime UTC from unix seconds. */
static void
wr_qdatetime_utc (GByteArray *a, gint64 unix_s)
{
  if (unix_s <= 0)
    {
      wr_i64 (a, 0);
      wr_u32 (a, 0);
      wr_u8 (a, 1); /* UTC */
      return;
    }
  GDateTime *dt = g_date_time_new_from_unix_utc (unix_s);
  if (!dt)
    {
      wr_i64 (a, 0);
      wr_u32 (a, 0);
      wr_u8 (a, 1);
      return;
    }
  int year = g_date_time_get_year (dt);
  int month = g_date_time_get_month (dt);
  int day = g_date_time_get_day_of_month (dt);
  int hour = g_date_time_get_hour (dt);
  int minute = g_date_time_get_minute (dt);
  int second = g_date_time_get_second (dt);
  g_date_time_unref (dt);

  /* Civil → Julian day (same algorithm as Qt QDate::toJulianDay). */
  int adj = (14 - month) / 12;
  int y = year + 4800 - adj;
  int m = month + 12 * adj - 3;
  gint64 jd = day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 -
              32045;
  guint32 ms = (guint32) ((hour * 3600 + minute * 60 + second) * 1000);
  wr_i64 (a, jd);
  wr_u32 (a, ms);
  wr_u8 (a, 1); /* UTC */
}

static GByteArray *
begin_msg (guint32 type, const char *id)
{
  GByteArray *a = g_byte_array_sized_new (128);
  wr_u32 (a, LOGFL_WSJTX_MAGIC);
  wr_u32 (a, LOGFL_WSJTX_SCHEMA);
  wr_u32 (a, type);
  wr_utf8 (a, id ? id : "WSJT-X");
  return a;
}

static void
wr_u16 (GByteArray *a, guint16 v)
{
  guint8 b[2] = { (v >> 8) & 0xff, v & 0xff };
  g_byte_array_append (a, b, 2);
}

/* QColor RGB: cspec=1, components as quint16 with duplicated bytes (x*0x101). */
static void
wr_qcolor_rgb (GByteArray *a, gboolean valid, guint8 r, guint8 g, guint8 b)
{
  if (!valid)
    {
      wr_u8 (a, 0); /* Invalid */
      return;
    }
  wr_u8 (a, 1); /* Rgb */
  wr_u16 (a, (guint16) (0xff * 0x101)); /* alpha */
  wr_u16 (a, (guint16) (r * 0x101));
  wr_u16 (a, (guint16) (g * 0x101));
  wr_u16 (a, (guint16) (b * 0x101));
  wr_u16 (a, 0); /* pad */
}

GByteArray *
logfl_wsjtx_build_heartbeat (const char *id, guint32 max_schema,
                             const char *version, const char *revision)
{
  GByteArray *a = begin_msg (LOGFL_WSJTX_MSG_HEARTBEAT, id);
  wr_u32 (a, max_schema ? max_schema : LOGFL_WSJTX_SCHEMA);
  wr_utf8 (a, version ? version : LOGFL_VERSION);
  wr_utf8 (a, revision ? revision : "");
  return a;
}

GByteArray *
logfl_wsjtx_build_status (const char *id, guint64 dial_hz, const char *mode,
                          const char *dx_call, const char *de_call)
{
  GByteArray *a = begin_msg (LOGFL_WSJTX_MSG_STATUS, id);
  wr_u64 (a, dial_hz);
  wr_utf8 (a, mode ? mode : "FT8");
  wr_utf8 (a, dx_call);
  wr_utf8 (a, ""); /* report */
  wr_utf8 (a, mode ? mode : "FT8");
  wr_u8 (a, 0); /* tx_enabled */
  wr_u8 (a, 0); /* transmitting */
  wr_u8 (a, 0); /* decoding */
  wr_i32 (a, 0);
  wr_i32 (a, 0);
  wr_utf8 (a, de_call);
  wr_utf8 (a, ""); /* de_grid */
  wr_utf8 (a, ""); /* dx_grid */
  wr_u8 (a, 0);    /* watchdog */
  wr_utf8 (a, ""); /* submode */
  wr_u8 (a, 0);    /* fast */
  wr_u8 (a, 0);    /* special op */
  return a;
}

GByteArray *
logfl_wsjtx_build_qso_logged (const char *id, gint64 ts_on, gint64 ts_off,
                              const char *dx_call, const char *dx_grid,
                              guint64 freq_hz, const char *mode,
                              const char *rst_sent, const char *rst_rcvd,
                              const char *tx_power, const char *comment,
                              const char *name, const char *my_call,
                              const char *my_grid)
{
  GByteArray *a = begin_msg (LOGFL_WSJTX_MSG_QSO_LOGGED, id);
  wr_qdatetime_utc (a, ts_off > 0 ? ts_off : ts_on);
  wr_utf8 (a, dx_call);
  wr_utf8 (a, dx_grid);
  wr_u64 (a, freq_hz);
  wr_utf8 (a, mode);
  wr_utf8 (a, rst_sent);
  wr_utf8 (a, rst_rcvd);
  wr_utf8 (a, tx_power);
  wr_utf8 (a, comment);
  wr_utf8 (a, name);
  wr_qdatetime_utc (a, ts_on > 0 ? ts_on : ts_off);
  wr_utf8 (a, my_call); /* operator */
  wr_utf8 (a, my_call);
  wr_utf8 (a, my_grid);
  wr_utf8 (a, ""); /* exch sent */
  wr_utf8 (a, ""); /* exch rcvd */
  return a;
}

GByteArray *
logfl_wsjtx_build_highlight (const char *id, const char *call,
                             gboolean bg_valid, guint8 bg_r, guint8 bg_g,
                             guint8 bg_b, gboolean fg_valid, guint8 fg_r,
                             guint8 fg_g, guint8 fg_b, gboolean last_only)
{
  GByteArray *a = begin_msg (LOGFL_WSJTX_MSG_HIGHLIGHT, id);
  wr_utf8 (a, call);
  wr_qcolor_rgb (a, bg_valid, bg_r, bg_g, bg_b);
  wr_qcolor_rgb (a, fg_valid, fg_r, fg_g, fg_b);
  wr_u8 (a, last_only ? 1 : 0);
  return a;
}

/* --- server ------------------------------------------------------------ */

struct _LogflWsjtxServer {
  char               *host;
  guint16             port;
  GSocket            *sock;
  GSource            *source;
  GSocketAddress     *peer; /* last sender */
  char               *peer_id;
  LogflWsjtxLoggedCb  logged_cb;
  gpointer            logged_data;
  LogflWsjtxStatusCb  status_cb;
  gpointer            status_data;
};

LogflWsjtxServer *
logfl_wsjtx_server_new (const char *bind_host, guint16 port)
{
  LogflWsjtxServer *s = g_new0 (LogflWsjtxServer, 1);
  s->host = g_strdup (bind_host && *bind_host ? bind_host
                                              : LOGFL_WSJTX_DEFAULT_HOST);
  s->port = port ? port : LOGFL_WSJTX_DEFAULT_PORT;
  return s;
}

void
logfl_wsjtx_server_free (LogflWsjtxServer *s)
{
  if (!s)
    return;
  logfl_wsjtx_server_stop (s);
  g_free (s->host);
  g_free (s->peer_id);
  g_clear_object (&s->peer);
  g_free (s);
}

void
logfl_wsjtx_server_set_logged_cb (LogflWsjtxServer *s, LogflWsjtxLoggedCb cb,
                                  gpointer user_data)
{
  g_return_if_fail (s != NULL);
  s->logged_cb = cb;
  s->logged_data = user_data;
}

void
logfl_wsjtx_server_set_status_cb (LogflWsjtxServer *s, LogflWsjtxStatusCb cb,
                                  gpointer user_data)
{
  g_return_if_fail (s != NULL);
  s->status_cb = cb;
  s->status_data = user_data;
}

guint16
logfl_wsjtx_server_port (const LogflWsjtxServer *s)
{
  return s ? s->port : 0;
}

gboolean
logfl_wsjtx_server_is_running (const LogflWsjtxServer *s)
{
  return s && s->sock != NULL;
}

static void
remember_peer (LogflWsjtxServer *s, GSocketAddress *addr, const char *id)
{
  g_clear_object (&s->peer);
  if (addr)
    s->peer = g_object_ref (addr);
  g_free (s->peer_id);
  s->peer_id = id && *id ? g_strdup (id) : NULL;
}

static gboolean
on_udp (GSocket *sock, GIOCondition cond, gpointer user_data)
{
  LogflWsjtxServer *s = user_data;
  (void) sock;
  if (!(cond & G_IO_IN) || !s->sock)
    return G_SOURCE_CONTINUE;

  guint8 buf[4096];
  GSocketAddress *from = NULL;
  GError *err = NULL;
  gssize n = g_socket_receive_from (sock, &from, (gchar *) buf, sizeof buf,
                                    NULL, &err);
  if (n < 0)
    {
      g_clear_error (&err);
      g_clear_object (&from);
      return G_SOURCE_CONTINUE;
    }
  if (n < 12)
    {
      g_clear_object (&from);
      return G_SOURCE_CONTINUE;
    }

  LogflWsjtxHeader hdr = { 0 };
  gsize off = 0;
  if (!logfl_wsjtx_parse_header (buf, (gsize) n, &hdr, &off, NULL))
    {
      g_clear_object (&from);
      return G_SOURCE_CONTINUE;
    }
  remember_peer (s, from, hdr.id);
  g_clear_object (&from);

  switch (hdr.type)
    {
    case LOGFL_WSJTX_MSG_QSO_LOGGED:
      {
        LogflWsjtxQsoLogged raw = { 0 };
        if (logfl_wsjtx_parse_qso_logged (buf, (gsize) n, &raw, NULL))
          {
            LogflQso *q = logfl_wsjtx_to_qso (&raw);
            if (q && s->logged_cb)
              s->logged_cb (q, &raw, s->logged_data);
            else
              logfl_qso_free (q);
            logfl_wsjtx_qso_logged_clear (&raw);
          }
        break;
      }
    case LOGFL_WSJTX_MSG_STATUS:
      {
        LogflWsjtxStatus st = { 0 };
        if (logfl_wsjtx_parse_status (buf, (gsize) n, &st, NULL))
          {
            if (s->status_cb)
              s->status_cb (&st, s->status_data);
            logfl_wsjtx_status_clear (&st);
          }
        break;
      }
    case LOGFL_WSJTX_MSG_HEARTBEAT:
      {
        /* Schema negotiation reply — optional but polite. */
        GByteArray *hb =
            logfl_wsjtx_build_heartbeat ("log-for-linux", LOGFL_WSJTX_SCHEMA,
                                         LOGFL_VERSION, "");
        logfl_wsjtx_server_send (s, hb->data, hb->len, NULL);
        g_byte_array_unref (hb);
        break;
      }
    default:
      break;
    }
  logfl_wsjtx_header_clear (&hdr);
  return G_SOURCE_CONTINUE;
}

gboolean
logfl_wsjtx_server_start (LogflWsjtxServer *s, GError **error)
{
  g_return_val_if_fail (s != NULL, FALSE);
  if (s->sock)
    return TRUE;

  GError *err = NULL;
  GSocket *sock =
      g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
                    G_SOCKET_PROTOCOL_UDP, &err);
  if (!sock)
    {
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_BIND,
                   "UDP socket: %s", err ? err->message : "?");
      g_clear_error (&err);
      return FALSE;
    }
  g_socket_set_blocking (sock, FALSE);
  g_socket_set_option (sock, SOL_SOCKET, SO_REUSEADDR, 1, NULL);

  GInetAddress *ia = g_inet_address_new_from_string (s->host);
  if (!ia)
    {
      g_object_unref (sock);
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_BIND,
                   "bad bind host: %s", s->host);
      return FALSE;
    }
  GSocketAddress *addr = g_inet_socket_address_new (ia, s->port);
  g_object_unref (ia);
  if (!g_socket_bind (sock, addr, TRUE, &err))
    {
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_BIND,
                   "bind %s:%u: %s", s->host, s->port,
                   err ? err->message : "?");
      g_clear_error (&err);
      g_object_unref (addr);
      g_object_unref (sock);
      return FALSE;
    }
  g_object_unref (addr);

  GSource *src = g_socket_create_source (sock, G_IO_IN, NULL);
  g_source_set_callback (src, G_SOURCE_FUNC (on_udp), s, NULL);
  g_source_attach (src, NULL);

  s->sock = sock;
  s->source = src;
  return TRUE;
}

void
logfl_wsjtx_server_stop (LogflWsjtxServer *s)
{
  if (!s)
    return;
  if (s->source)
    {
      g_source_destroy (s->source);
      g_source_unref (s->source);
      s->source = NULL;
    }
  g_clear_object (&s->sock);
  g_clear_object (&s->peer);
  g_clear_pointer (&s->peer_id, g_free);
}

gboolean
logfl_wsjtx_server_send (LogflWsjtxServer *s, const guint8 *data, gsize len,
                         GError **error)
{
  g_return_val_if_fail (s != NULL, FALSE);
  if (!s->sock || !s->peer)
    {
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_IO,
                   "no WSJT-X peer yet");
      return FALSE;
    }
  GError *err = NULL;
  gssize n = g_socket_send_to (s->sock, s->peer, (const gchar *) data, len,
                               NULL, &err);
  if (n < 0)
    {
      g_set_error (error, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_IO, "%s",
                   err ? err->message : "send failed");
      g_clear_error (&err);
      return FALSE;
    }
  return TRUE;
}

gboolean
logfl_wsjtx_server_highlight_b4 (LogflWsjtxServer *s, const char *client_id,
                                 const char *call, guint n_total,
                                 GError **error)
{
  g_return_val_if_fail (s != NULL, FALSE);
  if (!call || !*call)
    return TRUE;
  const char *id = client_id && *client_id
                       ? client_id
                       : (s->peer_id ? s->peer_id : "WSJT-X");
  GByteArray *pkt;
  if (n_total == 0)
    {
      /* New call — green background, black text */
      pkt = logfl_wsjtx_build_highlight (id, call, TRUE, 0x90, 0xee, 0x90,
                                         TRUE, 0x00, 0x00, 0x00, FALSE);
    }
  else
    {
      /* Worked before — yellow background */
      pkt = logfl_wsjtx_build_highlight (id, call, TRUE, 0xff, 0xff, 0x66,
                                         TRUE, 0x00, 0x00, 0x00, FALSE);
    }
  gboolean ok = logfl_wsjtx_server_send (s, pkt->data, pkt->len, error);
  g_byte_array_unref (pkt);
  return ok;
}
