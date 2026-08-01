/*
 * log-dupq-test — offline gate for the worked/dup lookup service.
 * Parser cases + a live loopback round trip on an ephemeral port.
 * No radio, no GUI, no store (the verdict callback is mocked).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#include "dup_srv.h"

static void
test_parse (void)
{
  char *call = NULL, *mode = NULL;
  gint64 hz = 0;

  g_assert_true (
      logfl_dup_parse_query ("DUP? OK2ABC 14035200 CW", &call, &hz, &mode));
  g_assert_cmpstr (call, ==, "OK2ABC");
  g_assert_cmpint (hz, ==, 14035200);
  g_assert_cmpstr (mode, ==, "CW");
  g_free (call);
  g_free (mode);

  /* Trailing newline and repeated separators are fine. */
  g_assert_true (
      logfl_dup_parse_query ("dup?  W1AW\t7005000  CW\n", &call, &hz, &mode));
  g_assert_cmpstr (call, ==, "W1AW");
  g_assert_cmpint (hz, ==, 7005000);
  g_free (call);
  g_free (mode);

  /* Malformed: wrong verb, missing fields, junk/zero frequency. */
  g_assert_false (logfl_dup_parse_query ("SPOT OK1BR 7000000 CW",
                                         &call, &hz, &mode));
  g_assert_false (logfl_dup_parse_query ("DUP? OK1BR 7000000",
                                         &call, &hz, &mode));
  g_assert_false (logfl_dup_parse_query ("DUP? OK1BR x7000 CW",
                                         &call, &hz, &mode));
  g_assert_false (logfl_dup_parse_query ("DUP? OK1BR 0 CW",
                                         &call, &hz, &mode));
  g_assert_false (logfl_dup_parse_query ("DUP? OK1BR 7000000 CW extra",
                                         &call, &hz, &mode));
  g_assert_false (logfl_dup_parse_query ("", &call, &hz, &mode));
  g_assert_false (logfl_dup_parse_query (NULL, &call, &hz, &mode));

  g_assert_cmpstr (logfl_dup_verdict_str (LOGFL_DUP_NEW), ==, "NEW");
  g_assert_cmpstr (logfl_dup_verdict_str (LOGFL_DUP_B4), ==, "B4");
  g_assert_cmpstr (logfl_dup_verdict_str (LOGFL_DUP_DUP), ==, "DUP");
}

/* Mock verdict: record what arrived, answer by callsign. */
typedef struct {
  char   call[32];
  char   mode[16];
  gint64 hz;
  guint  n_queries;
} MockState;

static LogflDupVerdict
mock_query (const char *call, gint64 hz, const char *mode, gpointer user)
{
  MockState *st = user;
  g_strlcpy (st->call, call, sizeof st->call);
  g_strlcpy (st->mode, mode, sizeof st->mode);
  st->hz = hz;
  st->n_queries++;
  if (g_str_equal (call, "OK9DUP"))
    return LOGFL_DUP_DUP;
  if (g_str_equal (call, "OK9B4"))
    return LOGFL_DUP_B4;
  return LOGFL_DUP_NEW;
}

/* Pump the main context until a datagram lands on cli (or ~2 s pass).
 * Returns the owned stripped text ("" = nothing arrived). */
static char *
wait_datagram (GSocket *cli)
{
  char buf[256];
  gint64 deadline = g_get_monotonic_time () + 2 * G_USEC_PER_SEC;
  while (g_get_monotonic_time () < deadline)
    {
      g_main_context_iteration (NULL, FALSE);
      if (g_socket_condition_check (cli, G_IO_IN) & G_IO_IN)
        {
          gssize n = g_socket_receive (cli, buf, sizeof buf - 1, NULL, NULL);
          if (n > 0)
            {
              buf[n] = '\0';
              return g_strdup (g_strstrip (buf));
            }
        }
      g_usleep (2000);
    }
  return g_strdup ("");
}

/* Send one datagram to the server and wait for the reply. */
static char *
roundtrip (GSocket *cli, GSocketAddress *srv_addr, const char *req)
{
  g_assert_cmpint (
      g_socket_send_to (cli, srv_addr, req, strlen (req), NULL, NULL), >, 0);
  return wait_datagram (cli);
}

static void
test_loopback (void)
{
  MockState st = { { 0 }, { 0 }, 0, 0 };
  LogflDupSrv *s = logfl_dup_srv_new (NULL, 0);   /* ephemeral port */
  logfl_dup_srv_set_query_cb (s, mock_query, &st);
  GError *err = NULL;
  g_assert_true (logfl_dup_srv_start (s, &err));
  g_assert_no_error (err);
  g_assert_true (logfl_dup_srv_is_running (s));
  guint16 port = logfl_dup_srv_port (s);
  g_assert_cmpuint (port, !=, 0);

  GSocket *cli = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
                               G_SOCKET_PROTOCOL_UDP, NULL);
  g_assert_nonnull (cli);
  g_socket_set_blocking (cli, FALSE);
  GInetAddress *ia = g_inet_address_new_from_string ("127.0.0.1");
  GSocketAddress *sa = g_inet_socket_address_new (ia, port);
  g_object_unref (ia);

  char *r = roundtrip (cli, sa, "DUP? OK9NEW 14022000 CW");
  g_assert_cmpstr (r, ==, "NEW OK9NEW");
  g_free (r);
  g_assert_cmpstr (st.call, ==, "OK9NEW");
  g_assert_cmpint (st.hz, ==, 14022000);
  g_assert_cmpstr (st.mode, ==, "CW");

  r = roundtrip (cli, sa, "DUP? OK9B4 14022000 CW");
  g_assert_cmpstr (r, ==, "B4 OK9B4");
  g_free (r);

  r = roundtrip (cli, sa, "DUP? OK9DUP 3520000 CW\n");
  g_assert_cmpstr (r, ==, "DUP OK9DUP");
  g_free (r);

  /* Garbage: consumed silently, no reply, callback never fired. */
  guint before = st.n_queries;
  r = roundtrip (cli, sa, "HELLO WORLD");
  g_assert_cmpstr (r, ==, "");
  g_free (r);
  g_assert_cmpuint (st.n_queries, ==, before);

  /* A good query still answers after the garbage one. */
  r = roundtrip (cli, sa, "DUP? OK9NEW 7012000 CW");
  g_assert_cmpstr (r, ==, "NEW OK9NEW");
  g_free (r);

  /* PUSH: a verdict change reaches the peer unsolicited — same format. */
  logfl_dup_srv_notify (s, "OK9NEW", LOGFL_DUP_DUP);
  r = wait_datagram (cli);
  g_assert_cmpstr (r, ==, "DUP OK9NEW");
  g_free (r);

  /* A second registered peer gets the push too. */
  GSocket *cli2 = g_socket_new (G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
                                G_SOCKET_PROTOCOL_UDP, NULL);
  g_assert_nonnull (cli2);
  g_socket_set_blocking (cli2, FALSE);
  r = roundtrip (cli2, sa, "DUP? OK9B4 14022000 CW");
  g_assert_cmpstr (r, ==, "B4 OK9B4");
  g_free (r);
  logfl_dup_srv_notify (s, "OK9B4", LOGFL_DUP_DUP);
  r = wait_datagram (cli);
  g_assert_cmpstr (r, ==, "DUP OK9B4");
  g_free (r);
  r = wait_datagram (cli2);
  g_assert_cmpstr (r, ==, "DUP OK9B4");
  g_free (r);
  g_object_unref (cli2);

  g_object_unref (sa);
  g_object_unref (cli);
  logfl_dup_srv_free (s);
}

static void
test_notify_no_peers (void)
{
  /* Notify with nobody listening (and even before start) must be a no-op. */
  LogflDupSrv *s = logfl_dup_srv_new (NULL, 0);
  logfl_dup_srv_notify (s, "OK1BR", LOGFL_DUP_DUP);
  g_assert_true (logfl_dup_srv_start (s, NULL));
  logfl_dup_srv_notify (s, "OK1BR", LOGFL_DUP_DUP);
  logfl_dup_srv_notify (s, NULL, LOGFL_DUP_NEW);
  logfl_dup_srv_free (s);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/dupq/parse", test_parse);
  g_test_add_func ("/dupq/loopback", test_loopback);
  g_test_add_func ("/dupq/notify-no-peers", test_notify_no_peers);
  return g_test_run ();
}
