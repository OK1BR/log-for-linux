/*
 * log-udp-test — offline gate for WSJT-X UDP (M6). No WSJT-X, no GUI.
 *
 *   - pack/parse round-trip for QSO Logged and Status
 *   - to_qso maps freq→band, power, station call
 *   - UDP server on loopback receives a datagram and fires logged_cb
 *   - highlight packet builds without error
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include <gio/gio.h>
#include <string.h>

#include "log_store.h"
#include "wsjtx_udp.h"

/* Fixed epoch: 2024-06-15 12:34:56 UTC */
#define TS_ON  1718454896
#define TS_OFF 1718454956

static void
test_parse_qso_logged (void)
{
  GByteArray *pkt = logfl_wsjtx_build_qso_logged (
      "WSJT-X", TS_ON, TS_OFF, "K1ABC", "FN42", 14074000, "FT8", "-10",
      "-12", "5", "CQ test", "Bob", "OK1BR", "JO70");
  g_assert_nonnull (pkt);

  LogflWsjtxQsoLogged m = { 0 };
  GError *err = NULL;
  g_assert_true (logfl_wsjtx_parse_qso_logged (pkt->data, pkt->len, &m, &err));
  g_assert_no_error (err);
  g_assert_cmpuint (m.hdr.type, ==, LOGFL_WSJTX_MSG_QSO_LOGGED);
  g_assert_cmpstr (m.hdr.id, ==, "WSJT-X");
  g_assert_cmpstr (m.dx_call, ==, "K1ABC");
  g_assert_cmpstr (m.dx_grid, ==, "FN42");
  g_assert_cmpuint (m.freq_hz, ==, 14074000);
  g_assert_cmpstr (m.mode, ==, "FT8");
  g_assert_cmpstr (m.rst_sent, ==, "-10");
  g_assert_cmpstr (m.rst_rcvd, ==, "-12");
  g_assert_cmpstr (m.tx_power, ==, "5");
  g_assert_cmpstr (m.comment, ==, "CQ test");
  g_assert_cmpstr (m.name, ==, "Bob");
  g_assert_cmpstr (m.my_call, ==, "OK1BR");
  g_assert_cmpstr (m.my_grid, ==, "JO70");
  g_assert_cmpint (m.ts_on, ==, TS_ON);
  g_assert_cmpint (m.ts_off, ==, TS_OFF);

  logfl_wsjtx_qso_logged_clear (&m);
  g_byte_array_unref (pkt);
}

static void
test_to_qso (void)
{
  GByteArray *pkt = logfl_wsjtx_build_qso_logged (
      "WSJT-X", TS_ON, TS_OFF, "dl2xyz", "JN48", 7074000, "FT4", "+02",
      "-08", "10W", NULL, NULL, "OK1BR", "JO70");
  LogflWsjtxQsoLogged m = { 0 };
  g_assert_true (logfl_wsjtx_parse_qso_logged (pkt->data, pkt->len, &m, NULL));

  LogflQso *q = logfl_wsjtx_to_qso (&m);
  g_assert_nonnull (q);
  g_assert_cmpstr (q->call, ==, "dl2xyz"); /* normalize happens on store_add */
  g_assert_cmpstr (q->mode, ==, "FT4");
  g_assert_cmpstr (q->band, ==, "40m");
  g_assert_cmpfloat (q->freq, ==, 7.074);
  g_assert_cmpstr (q->rst_sent, ==, "+02");
  g_assert_cmpstr (q->rst_rcvd, ==, "-08");
  g_assert_cmpfloat (q->tx_pwr, ==, 10.0);
  g_assert_cmpstr (q->station_callsign, ==, "OK1BR");
  g_assert_cmpstr (q->my_gridsquare, ==, "JO70");
  g_assert_cmpstr (q->gridsquare, ==, "JN48");
  g_assert_cmpint (q->ts, ==, TS_ON);

  /* Insert into store + dup skip */
  GError *err = NULL;
  LogflStore *s = logfl_store_open (":memory:", &err);
  g_assert_no_error (err);
  g_assert_true (logfl_store_add (s, q, &err));
  g_assert_no_error (err);

  gboolean is_dup = FALSE;
  g_assert_true (logfl_store_dup_check (s, q->call, q->band, q->mode, q->ts, 0,
                                        &is_dup, &err));
  g_assert_true (is_dup);

  LogflStoreStats st = { 0 };
  g_assert_true (logfl_store_stats (s, &st, &err));
  g_assert_cmpuint (st.n_qso, ==, 1);

  logfl_qso_free (q);
  logfl_wsjtx_qso_logged_clear (&m);
  g_byte_array_unref (pkt);
  logfl_store_close (s);
}

static void
test_parse_status (void)
{
  GByteArray *pkt =
      logfl_wsjtx_build_status ("WSJT-X", 14074000, "FT8", "W1AW", "OK1BR");
  LogflWsjtxStatus st = { 0 };
  GError *err = NULL;
  g_assert_true (logfl_wsjtx_parse_status (pkt->data, pkt->len, &st, &err));
  g_assert_no_error (err);
  g_assert_cmpuint (st.hdr.type, ==, LOGFL_WSJTX_MSG_STATUS);
  g_assert_cmpuint (st.dial_hz, ==, 14074000);
  g_assert_cmpstr (st.mode, ==, "FT8");
  g_assert_cmpstr (st.dx_call, ==, "W1AW");
  g_assert_cmpstr (st.de_call, ==, "OK1BR");
  logfl_wsjtx_status_clear (&st);
  g_byte_array_unref (pkt);
}

static void
test_bad_magic (void)
{
  guint8 junk[16] = { 0 };
  LogflWsjtxHeader h = { 0 };
  GError *err = NULL;
  g_assert_false (logfl_wsjtx_parse_header (junk, sizeof junk, &h, NULL, &err));
  g_assert_error (err, LOGFL_WSJTX_ERROR, LOGFL_WSJTX_ERROR_PARSE);
  g_clear_error (&err);
}

static void
test_highlight_build (void)
{
  GByteArray *pkt = logfl_wsjtx_build_highlight (
      "WSJT-X", "K1ABC", TRUE, 0xff, 0xff, 0x66, TRUE, 0, 0, 0, FALSE);
  g_assert_cmpuint (pkt->len, >, 20);
  LogflWsjtxHeader h = { 0 };
  g_assert_true (logfl_wsjtx_parse_header (pkt->data, pkt->len, &h, NULL, NULL));
  g_assert_cmpuint (h.type, ==, LOGFL_WSJTX_MSG_HIGHLIGHT);
  g_assert_cmpstr (h.id, ==, "WSJT-X");
  logfl_wsjtx_header_clear (&h);
  g_byte_array_unref (pkt);
}

/* --- live loopback server ---------------------------------------------- */

typedef struct {
  LogflQso *q;
  guint     n_status;
  char     *dx;
} UdpCap;

static void
on_logged (LogflQso *q, const LogflWsjtxQsoLogged *raw, gpointer user_data)
{
  (void) raw;
  UdpCap *c = user_data;
  c->q = q; /* take ownership */
}

static void
on_status (const LogflWsjtxStatus *st, gpointer user_data)
{
  UdpCap *c = user_data;
  c->n_status++;
  g_free (c->dx);
  c->dx = st->dx_call ? g_strdup (st->dx_call) : NULL;
}

static gboolean
quit_loop (gpointer data)
{
  g_main_loop_quit (data);
  return G_SOURCE_REMOVE;
}

static void
test_server_loopback (void)
{
  UdpCap cap = { 0 };
  /* Ephemeral port: bind 0 is not exposed — pick a high test port. */
  const guint16 port = 37237;
  LogflWsjtxServer *srv =
      logfl_wsjtx_server_new ("127.0.0.1", port);
  logfl_wsjtx_server_set_logged_cb (srv, on_logged, &cap);
  logfl_wsjtx_server_set_status_cb (srv, on_status, &cap);

  GError *err = NULL;
  if (!logfl_wsjtx_server_start (srv, &err))
    {
      /* Port busy — skip rather than fail the whole suite. */
      g_test_skip (err ? err->message : "bind failed");
      g_clear_error (&err);
      logfl_wsjtx_server_free (srv);
      return;
    }
  g_assert_true (logfl_wsjtx_server_is_running (srv));

  GByteArray *qso = logfl_wsjtx_build_qso_logged (
      "WSJT-X", TS_ON, TS_OFF, "PA0XYZ", "JO22", 21074000, "FT8", "-05",
      "-07", "20", NULL, NULL, "OK1BR", "JO70");
  GByteArray *st = logfl_wsjtx_build_status ("WSJT-X", 21074000, "FT8",
                                             "PA0XYZ", "OK1BR");

  GSocket *cli = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
                               G_SOCKET_PROTOCOL_UDP, &err);
  g_assert_no_error (err);
  GInetAddress *ia = g_inet_address_new_from_string ("127.0.0.1");
  GSocketAddress *to = g_inet_socket_address_new (ia, port);
  g_object_unref (ia);

  g_assert_cmpint (g_socket_send_to (cli, to, (gchar *) st->data, st->len,
                                     NULL, &err),
                   >, 0);
  g_assert_no_error (err);
  g_assert_cmpint (g_socket_send_to (cli, to, (gchar *) qso->data, qso->len,
                                     NULL, &err),
                   >, 0);
  g_assert_no_error (err);

  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  g_timeout_add (200, quit_loop, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  g_assert_cmpuint (cap.n_status, >=, 1);
  g_assert_cmpstr (cap.dx, ==, "PA0XYZ");
  g_assert_nonnull (cap.q);
  g_assert_cmpstr (cap.q->call, ==, "PA0XYZ");
  g_assert_cmpstr (cap.q->band, ==, "15m");
  g_assert_cmpstr (cap.q->mode, ==, "FT8");

  /* Highlight after peer is known */
  g_assert_true (
      logfl_wsjtx_server_highlight_b4 (srv, "WSJT-X", "PA0XYZ", 0, &err));
  g_assert_no_error (err);

  logfl_qso_free (cap.q);
  g_free (cap.dx);
  g_object_unref (to);
  g_object_unref (cli);
  g_byte_array_unref (qso);
  g_byte_array_unref (st);
  logfl_wsjtx_server_stop (srv);
  logfl_wsjtx_server_free (srv);
}

static void
test_empty_call_to_qso (void)
{
  GByteArray *pkt = logfl_wsjtx_build_qso_logged (
      "WSJT-X", TS_ON, TS_OFF, "", NULL, 14074000, "FT8", NULL, NULL, NULL,
      NULL, NULL, "OK1BR", NULL);
  LogflWsjtxQsoLogged m = { 0 };
  g_assert_true (logfl_wsjtx_parse_qso_logged (pkt->data, pkt->len, &m, NULL));
  g_assert_null (logfl_wsjtx_to_qso (&m));
  logfl_wsjtx_qso_logged_clear (&m);
  g_byte_array_unref (pkt);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/wsjtx/parse-qso-logged", test_parse_qso_logged);
  g_test_add_func ("/wsjtx/to-qso", test_to_qso);
  g_test_add_func ("/wsjtx/parse-status", test_parse_status);
  g_test_add_func ("/wsjtx/bad-magic", test_bad_magic);
  g_test_add_func ("/wsjtx/highlight-build", test_highlight_build);
  g_test_add_func ("/wsjtx/server-loopback", test_server_loopback);
  g_test_add_func ("/wsjtx/empty-call", test_empty_call_to_qso);
  return g_test_run ();
}
