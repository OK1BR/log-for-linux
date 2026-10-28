/*
 * log-tci-test — offline gate for the TCI client (M4). No radio, no GUI.
 *
 * Mock TCI server (libwebsockets, sdr-for-linux wire dialect) with the two
 * listeners the real server has since sdr-for-linux#46 — TCP on an
 * ephemeral 127.0.0.1 port and a Unix socket in a scratch directory —
 * drives logfl_tci_client over each transport in turn (#32):
 *   - handshake: batched init ending ready; → start() returns with
 *     protocol/device/vfo/mode filled,
 *   - live vfo: / modulation: broadcasts fire the state callback,
 *   - tune() queues a well-formed vfo:0,0,<hz>; command,
 *   - stop() leaves the client not-ready;
 * plus the server-string parser, the candidate order (a dead socket falls
 * through to TCP at once, not after the handshake timeout), and the default
 * order picking the socket under a scratch $XDG_RUNTIME_DIR.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <glib/gstdio.h>
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
static volatile gint s_push_rtty;
static volatile gint s_push_wpm;
static volatile gint s_push_spot;
static volatile gint s_push_nul;

/* Where the mock listens, in the client's own spelling. The socket lives
 * under a scratch $XDG_RUNTIME_DIR (set in main before GLib caches it) at
 * the path the client's default order looks at, so the auto test is the
 * real lookup. ⛔ Never the live runtime dir: lws unlinks whatever sits at
 * the path before it binds, and would steal a running radio's socket. */
static char  s_tcp_addr[64];                       /* ws://127.0.0.1:<port> */
static char  s_unix_addr[LOGFL_TCI_SOCK_PATH_MAX + 8];   /* unix:<path> */
static char  s_unix_path[LOGFL_TCI_SOCK_PATH_MAX]; /* lws keeps the pointer */
static char *s_runtime_dir;                        /* scratch XDG_RUNTIME_DIR */

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
          "cw_macros_speed:28;rx_enable:0,true;ready;start;");
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
      if (g_atomic_int_get (&s_push_rtty))
        {
          g_atomic_int_set (&s_push_rtty, 0);
          /* The radio switched itself to RTTY (family extension mode) —
           * the logbook's dropdown must follow, never sit on CW. */
          srv_queue_text ("modulation:0,rtty;");
        }
      if (g_atomic_int_get (&s_push_wpm))
        {
          g_atomic_int_set (&s_push_wpm, 0);
          srv_queue_text ("cw_macros_speed:34;");
        }
      if (g_atomic_int_get (&s_push_spot))
        {
          g_atomic_int_set (&s_push_spot, 0);
          /* Exactly what sdr-for-linux emits per click: both spellings,
           * plus junk that must never reach the operator's Call field. */
          srv_queue_text ("clicked_on_spot:NONSENSE,14025000;"
                          "rx_clicked_on_spot:1,0,DL9XY,14025000;"
                          "rx_clicked_on_spot:0,0,OK1BR/P,14025300;"
                          "clicked_on_spot:OK1BR/P,14025300;");
        }
      if (g_atomic_int_get (&s_push_nul))
        {
          g_atomic_int_set (&s_push_nul, 0);
          /* A frame with an embedded NUL, then a live update: the client
           * must survive the NUL and keep parsing commands after it. */
          static const char raw[] = { 'g', 'a', 'r', '\0', 'b', 'a', 'g',
                                      ';' };
          Msg *m = g_new0 (Msg, 1);
          m->data = g_memdup2 (raw, sizeof raw);
          m->len = sizeof raw;
          g_mutex_lock (&s_lock);
          g_queue_push_tail (&s_out, m);
          g_mutex_unlock (&s_lock);
          srv_queue_text ("vfo:0,0,7101500;");
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

static GThread *s_thread;

/* Both listeners in one context, the way sdr-for-linux binds them: explicit
 * vhosts, the protocol table on each. TCP on 127.0.0.1 only (a gate must
 * not open a LAN port) at an ephemeral port — a fixed one EADDRINUSEs on
 * re-runs while TIME_WAIT lingers. */
static void
mock_start (void)
{
  g_mutex_init (&s_lock);
  g_queue_init (&s_out);
  s_rx = g_string_new (NULL);
  g_atomic_int_set (&s_run, 1);
  g_atomic_int_set (&s_push_vfo, 0);
  g_atomic_int_set (&s_push_mode, 0);
  g_atomic_int_set (&s_push_rtty, 0);
  g_atomic_int_set (&s_push_wpm, 0);
  g_atomic_int_set (&s_push_spot, 0);
  g_atomic_int_set (&s_push_nul, 0);
  s_wsi = NULL;

  lws_set_log_level (LLL_ERR, NULL);
  struct lws_context_creation_info info;
  memset (&info, 0, sizeof info);
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
  info.gid = (gid_t) -1;
  info.uid = (uid_t) -1;
  s_ctx = lws_create_context (&info);
  g_assert_nonnull (s_ctx);

  struct lws_context_creation_info vi;
  memset (&vi, 0, sizeof vi);
  vi.vhost_name = "tcp";
  vi.port = 0;
  vi.iface = "127.0.0.1";
  vi.options = LWS_SERVER_OPTION_DISABLE_IPV6 |
               LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;
  vi.protocols = s_protocols;
  struct lws_vhost *vh_tcp = lws_create_vhost (s_ctx, &vi);
  g_assert_nonnull (vh_tcp);
  int port = lws_get_vhost_listen_port (vh_tcp);
  g_assert_cmpint (port, >, 0);
  g_snprintf (s_tcp_addr, sizeof s_tcp_addr, "ws://127.0.0.1:%d", port);

  /* The socket at the client's default path under the scratch runtime
   * dir; lws keeps the iface pointer and unlinks it at destroy, so the
   * path lives in a static. */
  char *dir = g_build_filename (s_runtime_dir, "sdr-for-linux", NULL);
  g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
  char *sock = logfl_tci_default_socket_path ();
  g_assert_true (g_str_has_prefix (sock, s_runtime_dir));
  g_assert_cmpuint (strlen (sock), <, sizeof s_unix_path);
  g_strlcpy (s_unix_path, sock, sizeof s_unix_path);
  g_snprintf (s_unix_addr, sizeof s_unix_addr, "unix:%s", s_unix_path);
  g_free (sock);
  g_free (dir);
  memset (&vi, 0, sizeof vi);
  vi.vhost_name = "unix";
  vi.port = 0;
  vi.iface = s_unix_path;
  vi.options = LWS_SERVER_OPTION_UNIX_SOCK;
  vi.protocols = s_protocols;
  struct lws_vhost *vh_unix = lws_create_vhost (s_ctx, &vi);
  g_assert_nonnull (vh_unix);
  g_assert_true (g_file_test (s_unix_path, G_FILE_TEST_EXISTS));

  s_thread = g_thread_new ("mock-tci", server_thread, NULL);
}

static void
mock_stop (void)
{
  g_atomic_int_set (&s_run, 0);
  lws_cancel_service (s_ctx);
  g_thread_join (s_thread);
  s_thread = NULL;
  lws_context_destroy (s_ctx);
  s_ctx = NULL;
  /* lws unlinks the socket file at destroy — the real server relies on
   * the same, so a stale file never survives a clean stop. */
  g_assert_false (g_file_test (s_unix_path, G_FILE_TEST_EXISTS));

  Msg *m;
  while ((m = g_queue_pop_head (&s_out)) != NULL)
    {
      g_free (m->data);
      g_free (m);
    }
  g_string_free (s_rx, TRUE);
  s_rx = NULL;
  g_mutex_clear (&s_lock);
}

/* ---- client capture ---------------------------------------------------- */

static GMutex c_lock;
static volatile gint c_states;
static double c_last_vfo;
static char c_last_mode[32];
static gint c_last_wpm;
static GPtrArray *c_spots;      /* "CALL@HZ" per spot callback */
static volatile gint c_closed;

static void
on_state (const LogflTciState *st, gpointer user_data)
{
  (void) user_data;
  g_mutex_lock (&c_lock);
  c_last_vfo = st->vfo_hz;
  g_strlcpy (c_last_mode, st->mode, sizeof c_last_mode);
  c_last_wpm = st->cw_wpm;
  g_mutex_unlock (&c_lock);
  g_atomic_int_inc (&c_states);
}

static void
on_spot (const char *call, double freq_hz, gpointer user_data)
{
  (void) user_data;
  g_mutex_lock (&c_lock);
  g_ptr_array_add (c_spots,
                   g_strdup_printf ("%s@%.0f", call, freq_hz));
  g_mutex_unlock (&c_lock);
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
  g_assert_cmpstr (logfl_tci_mode_to_log ("rtty"), ==, "RTTY");
  g_assert_cmpstr (logfl_tci_mode_to_log ("RTTY"), ==, "RTTY");
  g_assert_null (logfl_tci_mode_to_log ("nonsense"));
  g_assert_null (logfl_tci_mode_to_log (NULL));
}

/* The three spellings the Server row takes, and what it must refuse. */
static void
test_endpoint_parse (void)
{
  LogflTciEndpoint ep;
  char *s;

  g_assert_true (logfl_tci_endpoint_parse (
      "unix:/run/user/1000/sdr-for-linux/tci.sock", &ep));
  g_assert_true (ep.unix_sock);
  g_assert_cmpstr (ep.path, ==, "/run/user/1000/sdr-for-linux/tci.sock");
  s = logfl_tci_endpoint_to_string (&ep);
  g_assert_cmpstr (s, ==, "unix:/run/user/1000/sdr-for-linux/tci.sock");
  g_free (s);

  g_assert_true (logfl_tci_endpoint_parse ("ws://192.168.1.20:40001", &ep));
  g_assert_false (ep.unix_sock);
  g_assert_cmpstr (ep.host, ==, "192.168.1.20");
  g_assert_cmpint (ep.port, ==, 40001);

  /* Port defaults, a pasted URL's trailing slash, surrounding blanks. */
  g_assert_true (logfl_tci_endpoint_parse ("ws://radio.lan", &ep));
  g_assert_cmpstr (ep.host, ==, "radio.lan");
  g_assert_cmpint (ep.port, ==, LOGFL_TCI_DEFAULT_PORT);
  g_assert_true (logfl_tci_endpoint_parse ("  ws://radio.lan:40002/  ", &ep));
  g_assert_cmpstr (ep.host, ==, "radio.lan");
  g_assert_cmpint (ep.port, ==, 40002);

  /* The old Host field's spelling: bare host, bare host:port. */
  g_assert_true (logfl_tci_endpoint_parse ("127.0.0.1", &ep));
  g_assert_false (ep.unix_sock);
  g_assert_cmpstr (ep.host, ==, "127.0.0.1");
  g_assert_cmpint (ep.port, ==, LOGFL_TCI_DEFAULT_PORT);
  g_assert_true (logfl_tci_endpoint_parse ("10.0.0.5:40005", &ep));
  g_assert_cmpstr (ep.host, ==, "10.0.0.5");
  g_assert_cmpint (ep.port, ==, 40005);
  s = logfl_tci_endpoint_to_string (&ep);
  g_assert_cmpstr (s, ==, "ws://10.0.0.5:40005");
  g_free (s);

  /* Refused: empty, relative or abstract socket, TLS, bad port, a path
   * where a host goes, a socket path longer than sun_path. */
  memset (&ep, 0xAA, sizeof ep);
  g_assert_false (logfl_tci_endpoint_parse ("", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("   ", &ep));
  g_assert_false (logfl_tci_endpoint_parse (NULL, &ep));
  g_assert_false (logfl_tci_endpoint_parse ("unix:", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("unix:tci.sock", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("unix:@tci", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("wss://127.0.0.1:40001", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("http://127.0.0.1:40001", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("ws://127.0.0.1:0", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("ws://127.0.0.1:70000", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("ws://127.0.0.1:abc", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("ws://127.0.0.1:", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("ws://", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("ws://host/path", &ep));
  g_assert_false (logfl_tci_endpoint_parse ("/run/user/1000/tci.sock", &ep));
  char *longpath = g_strnfill (LOGFL_TCI_SOCK_PATH_MAX + 4, 'x');
  longpath[0] = '/';
  char *longaddr = g_strdup_printf ("unix:%s", longpath);
  g_assert_false (logfl_tci_endpoint_parse (longaddr, &ep));
  g_free (longaddr);
  g_free (longpath);
  /* A refusal leaves *out alone. */
  g_assert_cmpint ((unsigned char) ep.path[0], ==, 0xAA);

  /* The default socket path is the radio's: under the runtime dir. */
  char *def = logfl_tci_default_socket_path ();
  g_assert_true (g_str_has_suffix (def, "/sdr-for-linux/tci.sock"));
  g_assert_true (g_str_has_prefix (def, g_get_user_runtime_dir ()));
  g_free (def);
}

/* The whole handshake / live / tune suite over one transport — the mock's
 * TCP port or its Unix socket, by the test path's data. The two runs must
 * see the same wire behaviour: WebSocket framing does not care. */
static void
test_handshake_and_live (gconstpointer data)
{
  const char *transport = data;
  mock_start ();
  const char *server = strcmp (transport, "unix") == 0 ? s_unix_addr
                                                        : s_tcp_addr;

  g_atomic_int_set (&c_states, 0);
  g_atomic_int_set (&c_closed, 0);
  c_last_vfo = 0;
  c_last_mode[0] = '\0';
  c_spots = g_ptr_array_new_with_free_func (g_free);
  g_mutex_init (&c_lock);

  LogflTciClient *cli = logfl_tci_client_new (server);
  logfl_tci_client_set_state_cb (cli, on_state, NULL);
  logfl_tci_client_set_closed_cb (cli, on_closed, NULL);
  logfl_tci_client_set_spot_cb (cli, on_spot, NULL);

  g_assert_null (logfl_tci_client_endpoint (cli));
  GError *err = NULL;
  g_assert_true (logfl_tci_client_start (cli, &err));
  g_assert_no_error (err);
  g_assert_true (logfl_tci_client_is_ready (cli));
  /* The link reports the address it took, in the canonical spelling. */
  g_assert_cmpstr (logfl_tci_client_endpoint (cli), ==, server);

  /* Initial emit_state after handshake. */
  g_assert_true (wait_states (1, 2000));

  LogflTciState st;
  logfl_tci_client_get_state (cli, &st);
  g_assert_cmpstr (st.device, ==, "MockLogSDR");
  g_assert_true (g_str_has_prefix (st.protocol, "ExpertSDR3"));
  g_assert_cmpfloat (st.vfo_hz, ==, 7023456.0);
  g_assert_cmpstr (st.mode, ==, "cw");
  /* Keyer speed arrives in the handshake batch — Page Up/Down needs a
   * starting point the moment the radio is up. */
  g_assert_cmpint (st.cw_wpm, ==, 28);
  g_assert_cmpint (logfl_tci_client_cw_speed (cli), ==, 28);

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

  /* Live RTTY mode push — when the SDR lights up rtty, the state callback
   * must carry it (the UI maps it via logfl_tci_mode_to_log and follows;
   * a stale CW dropdown would key Morse into an FSK over). */
  before = g_atomic_int_get (&c_states);
  g_atomic_int_set (&s_push_rtty, 1);
  lws_cancel_service (s_ctx);
  g_assert_true (wait_states (before + 1, 2000));
  g_mutex_lock (&c_lock);
  g_assert_cmpstr (c_last_mode, ==, "rtty");
  g_mutex_unlock (&c_lock);

  /* Live keyer-speed push (radio-side change, e.g. the SDR's own control). */
  before = g_atomic_int_get (&c_states);
  g_atomic_int_set (&s_push_wpm, 1);
  lws_cancel_service (s_ctx);
  g_assert_true (wait_states (before + 1, 2000));
  g_mutex_lock (&c_lock);
  g_assert_cmpint (c_last_wpm, ==, 34);
  g_mutex_unlock (&c_lock);
  g_assert_cmpint (logfl_tci_client_cw_speed (cli), ==, 34);

  /* NUL resilience: a frame with an embedded NUL must not wedge the ';'
   * scan — the VFO update queued right behind it still has to land. */
  before = g_atomic_int_get (&c_states);
  g_atomic_int_set (&s_push_nul, 1);
  lws_cancel_service (s_ctx);
  g_assert_true (wait_states (before + 1, 2000));
  g_mutex_lock (&c_lock);
  g_assert_cmpfloat (c_last_vfo, ==, 7101500.0);
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

  /* Page Up/Down path: speed is sent as an absolute value and clamped to
   * what the radio accepts (sdr-for-linux caps at 60 WPM). */
  logfl_tci_client_cw_set_speed (cli, 35);
  logfl_tci_client_cw_set_speed (cli, 999);
  gboolean saw_speed = FALSE, saw_clamp = FALSE;
  for (int i = 0; i < 500; i++)
    {
      g_mutex_lock (&s_lock);
      if (strstr (s_rx->str, "cw_macros_speed:35;"))
        saw_speed = TRUE;
      if (strstr (s_rx->str, "cw_macros_speed:60;"))
        saw_clamp = TRUE;
      g_mutex_unlock (&s_lock);
      if (saw_speed && saw_clamp)
        break;
      g_usleep (5000);
    }
  g_assert_true (saw_speed);
  g_assert_true (saw_clamp);

  /* RTTY keying — the rtty_macros family with the
   * exact CW-path treatment: reserved ':'/','/';' scrubbed to spaces and
   * the deliberate leading space kept (word-gap rule, see tci_client.c). */
  logfl_tci_client_rtty_send (cli, "DL1ABC 599:001;");
  logfl_tci_client_rtty_stop (cli);
  gboolean saw_rtty = FALSE, saw_rtty_stop = FALSE;
  for (int i = 0; i < 500; i++)
    {
      g_mutex_lock (&s_lock);
      if (strstr (s_rx->str, "rtty_macros:0, DL1ABC 599 001 ;"))
        saw_rtty = TRUE;
      if (strstr (s_rx->str, "rtty_macros_stop;"))
        saw_rtty_stop = TRUE;
      g_mutex_unlock (&s_lock);
      if (saw_rtty && saw_rtty_stop)
        break;
      g_usleep (5000);
    }
  g_assert_true (saw_rtty);
  g_assert_true (saw_rtty_stop);

  /* Spot click on the panadapter → the call for the entry-row prefill.
   * sdr-for-linux sends both spellings per click, so two callbacks for the
   * one real click are expected (the UI keeps the first, see win.c). Junk
   * and other receivers must be dropped outright. */
  g_atomic_int_set (&s_push_spot, 1);
  lws_cancel_service (s_ctx);
  gboolean saw_spot = FALSE;
  for (int i = 0; i < 500; i++)
    {
      g_mutex_lock (&c_lock);
      saw_spot = c_spots->len >= 2;
      g_mutex_unlock (&c_lock);
      if (saw_spot)
        break;
      g_usleep (5000);
    }
  g_assert_true (saw_spot);
  g_mutex_lock (&c_lock);
  g_assert_cmpuint (c_spots->len, ==, 2);       /* junk + rx 1 dropped */
  g_assert_cmpstr (c_spots->pdata[0], ==, "OK1BR/P@14025300");
  g_assert_cmpstr (c_spots->pdata[1], ==, "OK1BR/P@14025300");
  g_mutex_unlock (&c_lock);

  logfl_tci_client_stop (cli);
  g_assert_false (logfl_tci_client_is_ready (cli));
  g_assert_null (logfl_tci_client_endpoint (cli));
  logfl_tci_client_free (cli);

  g_ptr_array_free (c_spots, TRUE);
  g_mutex_clear (&c_lock);
  mock_stop ();
}

/* The candidate order: a socket nobody serves fails inside connect(), so
 * the client is on the TCP candidate well inside the per-candidate
 * handshake timeout — the default order must not cost a radio on TCP
 * three seconds per reconnect. */
static void
test_order_fallback (void)
{
  mock_start ();
  char *absent = g_build_filename (s_runtime_dir, "nobody", "tci.sock", NULL);
  char *absent_addr = g_strdup_printf ("unix:%s", absent);
  const char *order[] = { absent_addr, s_tcp_addr, NULL };
  LogflTciClient *cli = logfl_tci_client_new_order (order);

  gint64 t0 = g_get_monotonic_time ();
  GError *err = NULL;
  g_assert_true (logfl_tci_client_start (cli, &err));
  g_assert_no_error (err);
  gint64 took_ms = (g_get_monotonic_time () - t0) / 1000;
  g_assert_cmpint (took_ms, <, 1500);
  g_assert_cmpstr (logfl_tci_client_endpoint (cli), ==, s_tcp_addr);
  g_assert_true (logfl_tci_client_is_ready (cli));

  logfl_tci_client_free (cli);
  g_free (absent_addr);
  g_free (absent);
  mock_stop ();
}

/* The default order with no server string: the socket at the radio's
 * default path wins (the mock's TCP port is ephemeral, so a wrong turn to
 * ws://127.0.0.1:40001 would find nothing — or, with a live radio on the
 * machine, a device that is not MockLogSDR). */
static void
test_default_order (void)
{
  mock_start ();
  LogflTciClient *cli = logfl_tci_client_new (NULL);
  GError *err = NULL;
  g_assert_true (logfl_tci_client_start (cli, &err));
  g_assert_no_error (err);
  g_assert_cmpstr (logfl_tci_client_endpoint (cli), ==, s_unix_addr);
  LogflTciState st;
  logfl_tci_client_get_state (cli, &st);
  g_assert_cmpstr (st.device, ==, "MockLogSDR");
  logfl_tci_client_free (cli);

  /* Blank is the same as NULL. */
  cli = logfl_tci_client_new ("   ");
  g_assert_true (logfl_tci_client_start (cli, &err));
  g_assert_no_error (err);
  g_assert_cmpstr (logfl_tci_client_endpoint (cli), ==, s_unix_addr);
  logfl_tci_client_free (cli);
  mock_stop ();
}

/* Nothing answers: the error names every candidate and why. A malformed
 * server string is its own error, before any dial. */
static void
test_connect_refused (void)
{
  char *absent = g_build_filename (s_runtime_dir, "nobody", "tci.sock", NULL);
  char *absent_addr = g_strdup_printf ("unix:%s", absent);
  const char *order[] = { absent_addr, "ws://127.0.0.1:9", NULL }; /* discard */
  LogflTciClient *cli = logfl_tci_client_new_order (order);
  GError *err = NULL;
  g_assert_false (logfl_tci_client_start (cli, &err));
  g_assert_nonnull (err);
  g_assert_nonnull (strstr (err->message, absent_addr));
  g_assert_nonnull (strstr (err->message, "ws://127.0.0.1:9 (refused)"));
  g_clear_error (&err);
  g_assert_false (logfl_tci_client_is_ready (cli));
  g_assert_null (logfl_tci_client_endpoint (cli));
  logfl_tci_client_free (cli);
  g_free (absent_addr);
  g_free (absent);

  cli = logfl_tci_client_new ("wss://127.0.0.1:40001");
  g_assert_false (logfl_tci_client_start (cli, &err));
  g_assert_nonnull (err);
  g_assert_nonnull (strstr (err->message, "malformed"));
  g_assert_nonnull (strstr (err->message, "wss://127.0.0.1:40001"));
  g_clear_error (&err);
  logfl_tci_client_free (cli);
}

int
main (int argc, char **argv)
{
  /* A scratch runtime dir, set before GLib reads XDG_RUNTIME_DIR, so the
   * client's default socket path lands in it — never the live one (see
   * mock_start). */
  s_runtime_dir = g_dir_make_tmp ("logfl-tci-XXXXXX", NULL);
  g_assert_nonnull (s_runtime_dir);
  g_setenv ("XDG_RUNTIME_DIR", s_runtime_dir, TRUE);

  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/tci/mode-map", test_mode_map);
  g_test_add_func ("/tci/endpoint-parse", test_endpoint_parse);
  g_test_add_data_func ("/tci/handshake-live-tune/tcp", "tcp",
                        test_handshake_and_live);
  g_test_add_data_func ("/tci/handshake-live-tune/unix", "unix",
                        test_handshake_and_live);
  g_test_add_func ("/tci/order-fallback", test_order_fallback);
  g_test_add_func ("/tci/default-order", test_default_order);
  g_test_add_func ("/tci/connect-refused", test_connect_refused);
  int rc = g_test_run ();

  /* Our own scratch: the socket is gone with the mock, the directories
   * follow (rmdir refuses anything not empty, which would be a leak). */
  char *dir = g_build_filename (s_runtime_dir, "sdr-for-linux", NULL);
  g_rmdir (dir);
  g_free (dir);
  g_rmdir (s_runtime_dir);
  g_free (s_runtime_dir);
  return rc;
}
