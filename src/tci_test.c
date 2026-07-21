/*
 * log-tci-test — offline gate for the TCI client (M4). No radio, no GUI.
 *
 * Mock TCI server (libwebsockets, sdr-for-linux wire dialect) on an
 * ephemeral 127.0.0.1 port drives logfl_tci_client:
 *   - handshake: batched init ending ready; → start() returns with
 *     protocol/device/vfo/mode filled,
 *   - live vfo: / modulation: broadcasts fire the state callback,
 *   - tune() queues a well-formed vfo:0,0,<hz>; command,
 *   - stop() leaves the client not-ready.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <libwebsockets.h>
#include <string.h>

#include "tci_client.h"

/* ---- mock TCI server --------------------------------------------------- */

typedef struct {
  char  *data;
  size_t len;
} Msg;

static GMutex s_lock;
static GQueue s_out;
static GString *s_rx;
static struct lws *s_wsi;
static struct lws_context *s_ctx;
static volatile gint s_run = 1;
static volatile gint s_push_vfo;
static volatile gint s_push_mode;

static void
srv_queue_text (const char *txt)
{
  Msg *m = g_new0 (Msg, 1);
  m->data = g_strdup (txt);
  m->len = strlen (txt);
  g_mutex_lock (&s_lock);
  g_queue_push_tail (&s_out, m);
  g_mutex_unlock (&s_lock);
}

static void
srv_exec (char *cmd)
{
  g_mutex_lock (&s_lock);
  g_string_append (s_rx, cmd);
  g_string_append_c (s_rx, ';');
  g_mutex_unlock (&s_lock);
}

static int
srv_cb (struct lws *wsi, enum lws_callback_reasons reason,
        void *user, void *in, size_t len)
{
  static GString *rxbuf;
  (void) user;
  switch (reason)
    {
    case LWS_CALLBACK_ESTABLISHED:
      s_wsi = wsi;
      if (!rxbuf)
        rxbuf = g_string_new (NULL);
      /* One batched init frame (real server may split; clients must cope). */
      srv_queue_text (
          "protocol:ExpertSDR3,1.9;device:MockLogSDR;receive_only:true;"
          "trx_count:1;channels_count:2;vfo_limits:0,61440000;"
          "modulations_list:am,lsb,usb,cw,cwl,cwu,digu,digl;"
          "dds:0,7020000;vfo:0,0,7023456;modulation:0,cw;"
          "rx_enable:0,true;ready;start;");
      lws_callback_on_writable (wsi);
      return 0;
    case LWS_CALLBACK_RECEIVE:
      {
        g_string_append_len (rxbuf, (const char *) in, (gssize) len);
        char *s = rxbuf->str, *semi;
        gsize used = 0;
        while ((semi = strchr (s, ';')) != NULL)
          {
            *semi = '\0';
            srv_exec (s);
            used = (gsize) (semi + 1 - rxbuf->str);
            s = semi + 1;
          }
        if (used)
          g_string_erase (rxbuf, 0, (gssize) used);
        return 0;
      }
    case LWS_CALLBACK_SERVER_WRITEABLE:
      {
        g_mutex_lock (&s_lock);
        Msg *m = g_queue_pop_head (&s_out);
        gboolean more = !g_queue_is_empty (&s_out);
        g_mutex_unlock (&s_lock);
        if (m)
          {
            unsigned char *buf = g_malloc (LWS_PRE + m->len);
            memcpy (buf + LWS_PRE, m->data, m->len);
            lws_write (wsi, buf + LWS_PRE, m->len, LWS_WRITE_TEXT);
            g_free (buf);
            g_free (m->data);
            g_free (m);
          }
        if (more)
          lws_callback_on_writable (wsi);
        return 0;
      }
    case LWS_CALLBACK_CLOSED:
      s_wsi = NULL;
      return 0;
    default:
      return 0;
    }
}

static const struct lws_protocols s_protocols[] = {
  { "tci", srv_cb, 0, 4096, 0, NULL, 0 },
  { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static gpointer
server_thread (gpointer data)
{
  (void) data;
  while (g_atomic_int_get (&s_run))
    {
      if (g_atomic_int_get (&s_push_vfo))
        {
          g_atomic_int_set (&s_push_vfo, 0);
          srv_queue_text ("vfo:0,0,14074000;");
        }
      if (g_atomic_int_get (&s_push_mode))
        {
          g_atomic_int_set (&s_push_mode, 0);
          srv_queue_text ("modulation:0,usb;");
        }
      g_mutex_lock (&s_lock);
      gboolean pending = !g_queue_is_empty (&s_out) && s_wsi;
      g_mutex_unlock (&s_lock);
      if (pending)
        lws_callback_on_writable (s_wsi);
      lws_service (s_ctx, 0);
      g_usleep (1000);
    }
  return NULL;
}

/* ---- client capture ---------------------------------------------------- */

static GMutex c_lock;
static volatile gint c_states;
static double c_last_vfo;
static char c_last_mode[32];
static volatile gint c_closed;

static void
on_state (const LogflTciState *st, gpointer user_data)
{
  (void) user_data;
  g_mutex_lock (&c_lock);
  c_last_vfo = st->vfo_hz;
  g_strlcpy (c_last_mode, st->mode, sizeof c_last_mode);
  g_mutex_unlock (&c_lock);
  g_atomic_int_inc (&c_states);
}

static void
on_closed (gpointer user_data)
{
  (void) user_data;
  g_atomic_int_set (&c_closed, 1);
}

static gboolean
wait_states (gint min, gint timeout_ms)
{
  gint64 deadline =
      g_get_monotonic_time () + timeout_ms * G_TIME_SPAN_MILLISECOND;
  while (g_atomic_int_get (&c_states) < min)
    {
      if (g_get_monotonic_time () > deadline)
        return FALSE;
      g_usleep (5000);
    }
  return TRUE;
}

/* ---- tests ------------------------------------------------------------- */

static void
test_mode_map (void)
{
  g_assert_cmpstr (logfl_tci_mode_to_log ("cw"), ==, "CW");
  g_assert_cmpstr (logfl_tci_mode_to_log ("CWU"), ==, "CW");
  g_assert_cmpstr (logfl_tci_mode_to_log ("usb"), ==, "SSB");
  g_assert_cmpstr (logfl_tci_mode_to_log ("lsb"), ==, "SSB");
  g_assert_cmpstr (logfl_tci_mode_to_log ("am"), ==, "AM");
  g_assert_cmpstr (logfl_tci_mode_to_log ("digu"), ==, "FT8");
  g_assert_null (logfl_tci_mode_to_log ("nonsense"));
  g_assert_null (logfl_tci_mode_to_log (NULL));
}

static void
test_handshake_and_live (void)
{
  g_mutex_init (&s_lock);
  g_queue_init (&s_out);
  s_rx = g_string_new (NULL);
  g_atomic_int_set (&s_run, 1);
  g_atomic_int_set (&s_push_vfo, 0);
  g_atomic_int_set (&s_push_mode, 0);
  g_atomic_int_set (&c_states, 0);
  g_atomic_int_set (&c_closed, 0);
  c_last_vfo = 0;
  c_last_mode[0] = '\0';
  g_mutex_init (&c_lock);

  lws_set_log_level (LLL_ERR, NULL);
  struct lws_context_creation_info info;
  memset (&info, 0, sizeof info);
  info.port = 0;               /* ephemeral — a fixed port EADDRINUSEs on
                                * re-runs while TIME_WAIT lingers */
  info.protocols = s_protocols;
  info.gid = (gid_t) -1;
  info.uid = (uid_t) -1;
  info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
  s_ctx = lws_create_context (&info);
  g_assert_nonnull (s_ctx);
  struct lws_vhost *vh = lws_get_vhost_by_name (s_ctx, "default");
  g_assert_nonnull (vh);
  int port = lws_get_vhost_listen_port (vh);
  g_assert_cmpint (port, >, 0);
  GThread *srv_thr = g_thread_new ("mock-tci", server_thread, NULL);

  LogflTciClient *cli = logfl_tci_client_new ("127.0.0.1", (guint16) port);
  logfl_tci_client_set_state_cb (cli, on_state, NULL);
  logfl_tci_client_set_closed_cb (cli, on_closed, NULL);

  GError *err = NULL;
  g_assert_true (logfl_tci_client_start (cli, &err));
  g_assert_no_error (err);
  g_assert_true (logfl_tci_client_is_ready (cli));

  /* Initial emit_state after handshake. */
  g_assert_true (wait_states (1, 2000));

  LogflTciState st;
  logfl_tci_client_get_state (cli, &st);
  g_assert_cmpstr (st.device, ==, "MockLogSDR");
  g_assert_true (g_str_has_prefix (st.protocol, "ExpertSDR3"));
  g_assert_cmpfloat (st.vfo_hz, ==, 7023456.0);
  g_assert_cmpstr (st.mode, ==, "cw");

  /* Live VFO push. lws_service() blocks until an event, so wake the server
   * loop explicitly — without it the flag sits until some internal timer. */
  gint before = g_atomic_int_get (&c_states);
  g_atomic_int_set (&s_push_vfo, 1);
  lws_cancel_service (s_ctx);
  g_assert_true (wait_states (before + 1, 2000));
  g_mutex_lock (&c_lock);
  g_assert_cmpfloat (c_last_vfo, ==, 14074000.0);
  g_mutex_unlock (&c_lock);

  /* Live mode push. */
  before = g_atomic_int_get (&c_states);
  g_atomic_int_set (&s_push_mode, 1);
  lws_cancel_service (s_ctx);
  g_assert_true (wait_states (before + 1, 2000));
  g_mutex_lock (&c_lock);
  g_assert_cmpstr (c_last_mode, ==, "usb");
  g_mutex_unlock (&c_lock);

  /* Explicit QSY. */
  logfl_tci_client_tune (cli, 21025000.0);
  gboolean saw_tune = FALSE;
  for (int i = 0; i < 500; i++)
    {
      g_mutex_lock (&s_lock);
      if (strstr (s_rx->str, "vfo:0,0,21025000;"))
        saw_tune = TRUE;
      g_mutex_unlock (&s_lock);
      if (saw_tune)
        break;
      g_usleep (5000);
    }
  g_assert_true (saw_tune);

  logfl_tci_client_stop (cli);
  g_assert_false (logfl_tci_client_is_ready (cli));
  logfl_tci_client_free (cli);

  g_atomic_int_set (&s_run, 0);
  lws_cancel_service (s_ctx);
  g_thread_join (srv_thr);
  lws_context_destroy (s_ctx);
  s_ctx = NULL;

  Msg *m;
  while ((m = g_queue_pop_head (&s_out)) != NULL)
    {
      g_free (m->data);
      g_free (m);
    }
  g_string_free (s_rx, TRUE);
  g_mutex_clear (&s_lock);
  g_mutex_clear (&c_lock);
}

static void
test_connect_refused (void)
{
  LogflTciClient *cli = logfl_tci_client_new ("127.0.0.1", 9); /* discard */
  GError *err = NULL;
  g_assert_false (logfl_tci_client_start (cli, &err));
  g_assert_nonnull (err);
  g_clear_error (&err);
  g_assert_false (logfl_tci_client_is_ready (cli));
  logfl_tci_client_free (cli);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/tci/mode-map", test_mode_map);
  g_test_add_func ("/tci/handshake-live-tune", test_handshake_and_live);
  g_test_add_func ("/tci/connect-refused", test_connect_refused);
  return g_test_run ();
}
