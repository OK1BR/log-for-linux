/* tci_client.c — TCI WebSocket client, text plane only (M4).
 *
 * libwebsockets client on its own service thread (skimmer / sdr-for-linux
 * pattern): lws_service loop, outgoing text queued and flushed on WRITEABLE,
 * incoming text accumulated and split on ';'. No binary Stream handling.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "tci_client.h"

#include <libwebsockets.h>
#include <stdlib.h>
#include <string.h>

#define LOGFL_TCI_ERROR      (g_quark_from_static_string ("logfl-tci-error"))
#define HANDSHAKE_TIMEOUT_S  3
#define TCI_TXT_MAX          (64 * 1024)   /* cap on unterminated rx text */

struct _LogflTciClient {
  char    *host;
  guint16  port;

  LogflTciStateCb  state_cb;
  gpointer         state_cb_data;
  LogflTciClosedCb closed_cb;
  gpointer         closed_cb_data;
  LogflTciSpotCb   spot_cb;
  gpointer         spot_cb_data;

  struct lws_context *ctx;
  struct lws         *wsi;
  GThread            *thread;

  GMutex lock;
  GCond  cond;
  gboolean up;
  gboolean ready;
  gboolean failed;
  GQueue   out;                /* outgoing text (char*) */

  double vfo_hz;
  char   mode[32];
  char   device[64];
  char   protocol[64];
  int    cw_wpm;               /* 0 until the radio reports it */

  GString *txt;                /* LWS thread only */
  volatile gint run;
  gboolean started;
};

/* ---- outgoing ---------------------------------------------------------- */

static void
cli_queue (LogflTciClient *c, char *msg /* takes ownership */)
{
  g_mutex_lock (&c->lock);
  g_queue_push_tail (&c->out, msg);
  g_mutex_unlock (&c->lock);
  if (c->ctx)
    lws_cancel_service (c->ctx);
}

/* ---- incoming text ----------------------------------------------------- */

static void
emit_state (LogflTciClient *c)
{
  LogflTciState st;
  g_mutex_lock (&c->lock);
  /* Snapshot cb+data under the lock — the setters run on the main thread
   * and null the pair on a live client (disconnect); a torn read here
   * would call through NULL or hand the callback stale user_data. */
  LogflTciStateCb cb = c->state_cb;
  gpointer cb_data = c->state_cb_data;
  st.vfo_hz = c->vfo_hz;
  g_strlcpy (st.mode, c->mode, sizeof st.mode);
  g_strlcpy (st.device, c->device, sizeof st.device);
  g_strlcpy (st.protocol, c->protocol, sizeof st.protocol);
  st.cw_wpm = c->cw_wpm;
  g_mutex_unlock (&c->lock);
  if (cb)
    cb (&st, cb_data);
}

/* A spot callsign is about to be typed into the log for the operator, so it
 * has to look like a callsign: letters/digits with optional /P-style
 * suffixes, at least one digit, no free text. Deliberately permissive about
 * exotic prefixes — the point is to reject decode garbage, not to referee
 * what is a valid call. */
static gboolean
spot_call_plausible (const char *s)
{
  gsize n = strlen (s);
  if (n < 3 || n > 16)
    return FALSE;
  gboolean digit = FALSE, alpha = FALSE;
  for (const char *p = s; *p; p++)
    {
      if (g_ascii_isdigit (*p))
        digit = TRUE;
      else if (g_ascii_isalpha (*p))
        alpha = TRUE;
      else if (*p != '/')
        return FALSE;
    }
  return digit && alpha;
}

static void
handle_command (LogflTciClient *c, char *cmd)
{
  char *args = strchr (cmd, ':');
  if (args)
    *args++ = '\0';
  for (char *p = cmd; *p; p++)
    *p = (char) g_ascii_tolower (*p);

  gboolean changed = FALSE;
  char     spot_call[32] = "";     /* non-empty → fire the spot callback */
  double   spot_hz = 0;

  g_mutex_lock (&c->lock);
  if (strcmp (cmd, "ready") == 0)
    {
      c->ready = TRUE;
      g_cond_broadcast (&c->cond);
    }
  else if (strcmp (cmd, "protocol") == 0 && args)
    {
      g_strlcpy (c->protocol, args, sizeof c->protocol);
    }
  else if (strcmp (cmd, "device") == 0 && args)
    {
      g_strlcpy (c->device, args, sizeof c->device);
    }
  else if (strcmp (cmd, "vfo") == 0 && args)
    {
      /* vfo:<rx>,<ch>,<hz> — track rx 0 channel A. */
      char *c1 = strchr (args, ',');
      char *c2 = c1 ? strchr (c1 + 1, ',') : NULL;
      if (c2 && strtol (args, NULL, 10) == 0 &&
          strtol (c1 + 1, NULL, 10) == 0)
        {
          double hz = g_ascii_strtod (c2 + 1, NULL);
          if (hz > 0 && hz != c->vfo_hz)
            {
              c->vfo_hz = hz;
              changed = TRUE;
            }
        }
    }
  else if (strcmp (cmd, "modulation") == 0 && args)
    {
      /* modulation:<rx>,<mode> — track receiver 0. */
      char *comma = strchr (args, ',');
      if (comma && strtol (args, NULL, 10) == 0)
        {
          char tmp[32];
          g_strlcpy (tmp, comma + 1, sizeof tmp);
          for (char *p = tmp; *p; p++)
            *p = (char) g_ascii_tolower (*p);
          if (tmp[0] && g_strcmp0 (tmp, c->mode) != 0)
            {
              g_strlcpy (c->mode, tmp, sizeof c->mode);
              changed = TRUE;
            }
        }
    }
  else if ((strcmp (cmd, "cw_macros_speed") == 0 ||
            strcmp (cmd, "cw_keyer_speed") == 0) && args)
    {
      /* cw_macros_speed:<wpm> — the radio's accepted value (also sent on
       * handshake and after our own set, so this is the authority). */
      int wpm = (int) strtol (args, NULL, 10);
      if (wpm > 0 && wpm != c->cw_wpm)
        {
          c->cw_wpm = wpm;
          changed = TRUE;
        }
    }
  else if ((strcmp (cmd, "rx_clicked_on_spot") == 0 ||
            strcmp (cmd, "clicked_on_spot") == 0) && args)
    {
      /* The operator clicked a spot on the panadapter. sdr-for-linux sends
       * both spellings for every click (tci_server.c): the rx_ form carries
       * <rx>,<ch> first, the legacy one starts at the call. Fields after the
       * frequency (none today) are ignored. */
      char **f = g_strsplit (args, ",", 0);
      guint n = g_strv_length (f);
      gboolean rx_form = strcmp (cmd, "rx_clicked_on_spot") == 0;
      guint i = rx_form ? 2 : 0;   /* index of the call field */
      if (n >= i + 2)
        {
          char *call = g_strstrip (f[i]);
          double hz = g_ascii_strtod (f[i + 1], NULL);
          /* rx_ form: receiver 0 channel A only, matching vfo/modulation. */
          if ((!rx_form || (strtol (f[0], NULL, 10) == 0 &&
                            strtol (f[1], NULL, 10) == 0)) &&
              hz > 0 && spot_call_plausible (call))
            {
              for (char *p = call; *p; p++)
                *p = (char) g_ascii_toupper (*p);
              g_strlcpy (spot_call, call, sizeof spot_call);
              spot_hz = hz;
            }
        }
      g_strfreev (f);
    }
  LogflTciSpotCb spot_cb = c->spot_cb;   /* snapshot, same as emit_state */
  gpointer spot_cb_data = c->spot_cb_data;
  g_mutex_unlock (&c->lock);

  if (changed)
    emit_state (c);
  if (spot_call[0] && spot_cb)
    spot_cb (spot_call, spot_hz, spot_cb_data);
}

static void
drain_text (LogflTciClient *c)
{
  /* Scan by length, not strchr: a stray NUL in a frame must not stop the
   * ';' search for good — that would wedge the text plane while the link
   * still looks ready (a NUL inside one command just truncates that one). */
  gsize used = 0;
  for (;;)
    {
      char *base = c->txt->str + used;
      char *semi = memchr (base, ';', c->txt->len - used);
      if (!semi)
        break;
      *semi = '\0';
      handle_command (c, base);
      used = (gsize) (semi + 1 - c->txt->str);
    }
  if (used)
    g_string_erase (c->txt, 0, (gssize) used);
  /* A peer that never sends ';' must not grow the buffer without bound. */
  if (c->txt->len > TCI_TXT_MAX)
    g_string_truncate (c->txt, 0);
}

/* ---- LWS --------------------------------------------------------------- */

static int
client_cb (struct lws *wsi, enum lws_callback_reasons reason,
           void *user, void *in, size_t len)
{
  LogflTciClient *c = lws_context_user (lws_get_context (wsi));
  (void) user;

  switch (reason)
    {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
      g_mutex_lock (&c->lock);
      c->wsi = wsi;
      c->up = TRUE;
      g_cond_broadcast (&c->cond);
      g_mutex_unlock (&c->lock);
      return 0;

    case LWS_CALLBACK_CLIENT_RECEIVE:
      /* Text only — ignore binary Stream frames from skimmers/IQ clients. */
      if (!lws_frame_is_binary (wsi))
        {
          g_string_append_len (c->txt, (const char *) in, (gssize) len);
          drain_text (c);
        }
      return 0;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
      {
        g_mutex_lock (&c->lock);
        char *msg = g_queue_pop_head (&c->out);
        gboolean more = !g_queue_is_empty (&c->out);
        g_mutex_unlock (&c->lock);
        if (msg)
          {
            size_t n = strlen (msg);
            unsigned char *buf = g_malloc (LWS_PRE + n);
            memcpy (buf + LWS_PRE, msg, n);
            lws_write (wsi, buf + LWS_PRE, n, LWS_WRITE_TEXT);
            g_free (buf);
            g_free (msg);
          }
        if (more)
          lws_callback_on_writable (wsi);
        return 0;
      }

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
      g_mutex_lock (&c->lock);
      c->failed = TRUE;
      g_cond_broadcast (&c->cond);
      g_mutex_unlock (&c->lock);
      return -1;

    case LWS_CALLBACK_CLIENT_CLOSED:
      {
        g_mutex_lock (&c->lock);
        c->up = FALSE;
        c->wsi = NULL;
        LogflTciClosedCb cb = c->closed_cb;   /* snapshot, see emit_state */
        gpointer cb_data = c->closed_cb_data;
        g_cond_broadcast (&c->cond);
        g_mutex_unlock (&c->lock);
        if (g_atomic_int_get (&c->run) && cb)
          cb (cb_data);
        return 0;
      }

    default:
      return 0;
    }
}

static const struct lws_protocols c_protocols[] = {
  { "tci", client_cb, 0, 4096, 0, NULL, 0 },
  { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static gpointer
service_thread (gpointer data)
{
  LogflTciClient *c = data;
  while (g_atomic_int_get (&c->run))
    {
      g_mutex_lock (&c->lock);
      gboolean pending = c->up && !g_queue_is_empty (&c->out);
      struct lws *wsi = c->wsi;
      g_mutex_unlock (&c->lock);
      if (pending && wsi)
        lws_callback_on_writable (wsi);
      lws_service (c->ctx, 0);
      g_usleep (1000);
    }
  return NULL;
}

/* ---- public API -------------------------------------------------------- */

LogflTciClient *
logfl_tci_client_new (const char *host, guint16 port)
{
  LogflTciClient *c = g_new0 (LogflTciClient, 1);
  c->host = g_strdup (host && *host ? host : LOGFL_TCI_DEFAULT_HOST);
  c->port = port ? port : LOGFL_TCI_DEFAULT_PORT;
  g_mutex_init (&c->lock);
  g_cond_init (&c->cond);
  g_queue_init (&c->out);
  c->txt = g_string_new (NULL);
  return c;
}

void
logfl_tci_client_free (LogflTciClient *c)
{
  if (!c)
    return;
  logfl_tci_client_stop (c);
  char *msg;
  while ((msg = g_queue_pop_head (&c->out)) != NULL)
    g_free (msg);
  g_string_free (c->txt, TRUE);
  g_mutex_clear (&c->lock);
  g_cond_clear (&c->cond);
  g_free (c->host);
  g_free (c);
}

/* The setters lock: win.c nulls the callbacks on a live, streaming client
 * before freeing it, and the LWS thread snapshots the pair under the same
 * lock (emit_state) — without it the thread can see a torn cb/data pair. */
void
logfl_tci_client_set_state_cb (LogflTciClient *c, LogflTciStateCb cb,
                               gpointer user_data)
{
  g_mutex_lock (&c->lock);
  c->state_cb = cb;
  c->state_cb_data = user_data;
  g_mutex_unlock (&c->lock);
}

void
logfl_tci_client_set_closed_cb (LogflTciClient *c, LogflTciClosedCb cb,
                                gpointer user_data)
{
  g_mutex_lock (&c->lock);
  c->closed_cb = cb;
  c->closed_cb_data = user_data;
  g_mutex_unlock (&c->lock);
}

void
logfl_tci_client_set_spot_cb (LogflTciClient *c, LogflTciSpotCb cb,
                              gpointer user_data)
{
  g_mutex_lock (&c->lock);
  c->spot_cb = cb;
  c->spot_cb_data = user_data;
  g_mutex_unlock (&c->lock);
}

gboolean
logfl_tci_client_start (LogflTciClient *c, GError **error)
{
  if (c->thread)
    {
      g_set_error (error, LOGFL_TCI_ERROR, 1, "TCI client already started");
      return FALSE;
    }

  lws_set_log_level (LLL_ERR, NULL);
  struct lws_context_creation_info info;
  memset (&info, 0, sizeof info);
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = c_protocols;
  info.gid = (gid_t) -1;
  info.uid = (uid_t) -1;
  info.user = c;
  c->ctx = lws_create_context (&info);
  if (!c->ctx)
    {
      g_set_error (error, LOGFL_TCI_ERROR, 2, "lws_create_context failed");
      return FALSE;
    }

  c->ready = c->failed = c->up = FALSE;
  c->vfo_hz = 0;
  c->mode[0] = c->device[0] = c->protocol[0] = '\0';
  g_string_set_size (c->txt, 0);

  struct lws_client_connect_info ci;
  memset (&ci, 0, sizeof ci);
  ci.context = c->ctx;
  ci.address = c->host;
  ci.port = c->port;
  ci.path = "/";
  ci.host = c->host;
  ci.origin = c->host;
  ci.protocol = "tci";
  lws_client_connect_via_info (&ci);

  g_atomic_int_set (&c->run, 1);
  c->thread = g_thread_new ("logfl-tci", service_thread, c);

  gint64 deadline =
      g_get_monotonic_time () + HANDSHAKE_TIMEOUT_S * G_TIME_SPAN_SECOND;
  g_mutex_lock (&c->lock);
  while (!c->ready && !c->failed)
    {
      if (!g_cond_wait_until (&c->cond, &c->lock, deadline))
        break;
    }
  gboolean ready = c->ready, failed = c->failed;
  g_mutex_unlock (&c->lock);

  if (!ready)
    {
      g_set_error (error, LOGFL_TCI_ERROR, 3,
                   failed ? "connection to ws://%s:%u refused"
                          : "handshake timeout against ws://%s:%u (no ready;)",
                   c->host, c->port);
      logfl_tci_client_stop (c);
      return FALSE;
    }

  c->started = TRUE;
  /* Push initial snapshot so the UI can prefill without waiting for a change. */
  emit_state (c);
  return TRUE;
}

void
logfl_tci_client_stop (LogflTciClient *c)
{
  if (!c || !c->thread)
    return;
  c->started = FALSE;
  g_atomic_int_set (&c->run, 0);
  if (c->ctx)
    lws_cancel_service (c->ctx);
  g_thread_join (c->thread);
  c->thread = NULL;
  if (c->ctx)
    {
      lws_context_destroy (c->ctx);
      c->ctx = NULL;
    }
  c->wsi = NULL;
  c->up = c->ready = c->failed = FALSE;
  g_string_set_size (c->txt, 0);
}

gboolean
logfl_tci_client_is_ready (LogflTciClient *c)
{
  if (!c)
    return FALSE;
  g_mutex_lock (&c->lock);
  gboolean r = c->ready && c->up;
  g_mutex_unlock (&c->lock);
  return r;
}

void
logfl_tci_client_get_state (LogflTciClient *c, LogflTciState *out)
{
  g_return_if_fail (out != NULL);
  g_mutex_lock (&c->lock);
  out->vfo_hz = c->vfo_hz;
  g_strlcpy (out->mode, c->mode, sizeof out->mode);
  g_strlcpy (out->device, c->device, sizeof out->device);
  g_strlcpy (out->protocol, c->protocol, sizeof out->protocol);
  out->cw_wpm = c->cw_wpm;
  g_mutex_unlock (&c->lock);
}

void
logfl_tci_client_tune (LogflTciClient *c, double freq_hz)
{
  if (!c || !c->thread || freq_hz <= 0)
    return;
  cli_queue (c, g_strdup_printf ("vfo:0,0,%lld;",
                                 (long long) (freq_hz + 0.5)));
}

void
logfl_tci_client_cw_send (LogflTciClient *c, const char *text)
{
  if (!c || !c->thread || !text || !*text)
    return;
  /* TCI reserves ':' ',' ';' — scrub free text before queueing. The LEADING
   * space is deliberate and mirrors what SDC sends: sdr-for-linux's CW
   * generator inserts the word gap between two queued messages only when the
   * SECOND one asks for it up front (F2, F3 fired back to back); on an idle
   * keyer the leading space is skipped, so an over never starts with dead
   * air. A trailing space does nothing there — tried and reverted. */
  char *t = g_strdup_printf (" %s", text);
  for (char *p = t; *p; p++)
    {
      if (*p == ':' || *p == ',' || *p == ';')
        *p = ' ';
    }
  /* ExpertSDR / sdr-for-linux: cw_macros:<rx>,<text> */
  cli_queue (c, g_strdup_printf ("cw_macros:0,%s;", t));
  g_free (t);
}

void
logfl_tci_client_cw_stop (LogflTciClient *c)
{
  if (!c || !c->thread)
    return;
  cli_queue (c, g_strdup ("cw_macros_stop;"));
}

void
logfl_tci_client_cw_set_speed (LogflTciClient *c, int wpm)
{
  if (!c || !c->thread)
    return;
  wpm = CLAMP (wpm, LOGFL_TCI_WPM_MIN, LOGFL_TCI_WPM_MAX);
  cli_queue (c, g_strdup_printf ("cw_macros_speed:%d;", wpm));
}

int
logfl_tci_client_cw_speed (LogflTciClient *c)
{
  if (!c)
    return 0;
  g_mutex_lock (&c->lock);
  int wpm = c->cw_wpm;
  g_mutex_unlock (&c->lock);
  return wpm;
}

const char *
logfl_tci_mode_to_log (const char *tci_mode)
{
  if (!tci_mode || !*tci_mode)
    return NULL;
  if (g_ascii_strcasecmp (tci_mode, "cw") == 0 ||
      g_ascii_strcasecmp (tci_mode, "cwu") == 0 ||
      g_ascii_strcasecmp (tci_mode, "cwl") == 0)
    return "CW";
  if (g_ascii_strcasecmp (tci_mode, "usb") == 0 ||
      g_ascii_strcasecmp (tci_mode, "lsb") == 0 ||
      g_ascii_strcasecmp (tci_mode, "ssb") == 0)
    return "SSB";
  if (g_ascii_strcasecmp (tci_mode, "am") == 0)
    return "AM";
  if (g_ascii_strcasecmp (tci_mode, "fm") == 0)
    return "FM";
  if (g_ascii_strcasecmp (tci_mode, "digu") == 0 ||
      g_ascii_strcasecmp (tci_mode, "digl") == 0)
    return "FT8";
  return NULL;
}
