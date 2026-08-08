/* dup_srv.c — worked/dup lookup service (see dup_srv.h for the protocol).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "dup_srv.h"

#include <gio/gio.h>
#include <string.h>

G_DEFINE_QUARK (logfl-dup-error-quark, logfl_dup_error)

/* Recent valid requesters — the audience for unsolicited verdict pushes.
 * A handful is plenty (one skimmer, maybe a second instance per mode). */
#define DUP_MAX_PEERS 8
#define DUP_PEER_TTL_US (10 * 60 * (gint64) G_USEC_PER_SEC)

typedef struct {
  GSocketAddress *addr;        /* NULL = free slot */
  gint64          seen;        /* monotonic µs of the last valid query */
} DupPeer;

struct _LogflDupSrv {
  char            *host;
  guint16          port;      /* requested; 0 = ephemeral */
  GSocket         *sock;
  GSource         *source;
  LogflDupQueryCb  query_cb;
  gpointer         query_data;
  DupPeer          peers[DUP_MAX_PEERS];
};

const char *
logfl_dup_verdict_str (LogflDupVerdict v)
{
  switch (v)
    {
    case LOGFL_DUP_B4:  return "B4";
    case LOGFL_DUP_DUP: return "DUP";
    case LOGFL_DUP_INV: return "INV";
    case LOGFL_DUP_NEW:
    default:            return "NEW";
    }
}

gboolean
logfl_dup_parse_query (const char *line, char **call, gint64 *hz, char **mode)
{
  if (call)
    *call = NULL;
  if (mode)
    *mode = NULL;
  if (hz)
    *hz = 0;
  if (!line)
    return FALSE;

  char **tok = g_strsplit_set (line, " \t\r\n", -1);
  guint n = 0;
  /* Compact: g_strsplit_set leaves empties between adjacent separators. */
  for (guint i = 0; tok[i]; i++)
    {
      if (*tok[i])
        tok[n++] = tok[i];
      else
        g_free (tok[i]);
      if (n <= i)
        tok[i] = NULL;
    }

  gboolean ok = FALSE;
  if (n == 4 && g_ascii_strcasecmp (tok[0], "DUP?") == 0)
    {
      char *end = NULL;
      gint64 f = g_ascii_strtoll (tok[2], &end, 10);
      if (end && *end == '\0' && f > 0)
        {
          if (call)
            *call = g_strdup (tok[1]);
          if (hz)
            *hz = f;
          if (mode)
            *mode = g_strdup (tok[3]);
          ok = TRUE;
        }
    }
  for (guint i = 0; i < n; i++)
    g_free (tok[i]);
  g_free (tok);
  return ok;
}

LogflDupSrv *
logfl_dup_srv_new (const char *bind_host, guint16 port)
{
  LogflDupSrv *s = g_new0 (LogflDupSrv, 1);
  s->host = g_strdup (bind_host && *bind_host ? bind_host
                                              : LOGFL_DUP_DEFAULT_HOST);
  s->port = port;
  return s;
}

void
logfl_dup_srv_free (LogflDupSrv *s)
{
  if (!s)
    return;
  logfl_dup_srv_stop (s);
  g_free (s->host);
  g_free (s);
}

void
logfl_dup_srv_set_query_cb (LogflDupSrv *s, LogflDupQueryCb cb,
                            gpointer user_data)
{
  g_return_if_fail (s != NULL);
  s->query_cb = cb;
  s->query_data = user_data;
}

gboolean
logfl_dup_srv_is_running (const LogflDupSrv *s)
{
  return s && s->sock != NULL;
}

guint16
logfl_dup_srv_port (const LogflDupSrv *s)
{
  if (!s)
    return 0;
  if (s->sock)
    {
      GSocketAddress *a = g_socket_get_local_address (s->sock, NULL);
      if (a)
        {
          guint16 p =
              g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (a));
          g_object_unref (a);
          return p;
        }
    }
  return s->port;
}

static gboolean
peer_addr_equal (GSocketAddress *a, GSocketAddress *b)
{
  if (!G_IS_INET_SOCKET_ADDRESS (a) || !G_IS_INET_SOCKET_ADDRESS (b))
    return FALSE;
  GInetSocketAddress *ia = G_INET_SOCKET_ADDRESS (a);
  GInetSocketAddress *ib = G_INET_SOCKET_ADDRESS (b);
  return g_inet_socket_address_get_port (ia) ==
             g_inet_socket_address_get_port (ib) &&
         g_inet_address_equal (g_inet_socket_address_get_address (ia),
                               g_inet_socket_address_get_address (ib));
}

static void
remember_peer (LogflDupSrv *s, GSocketAddress *addr)
{
  gint64 now = g_get_monotonic_time ();
  DupPeer *slot = &s->peers[0];
  for (guint i = 0; i < DUP_MAX_PEERS; i++)
    {
      DupPeer *p = &s->peers[i];
      if (p->addr && peer_addr_equal (p->addr, addr))
        {
          p->seen = now;
          return;
        }
      /* Free slot wins; otherwise remember the stalest for eviction. */
      if (!p->addr && slot->addr)
        slot = p;
      else if (p->addr && slot->addr && p->seen < slot->seen)
        slot = p;
    }
  g_clear_object (&slot->addr);
  slot->addr = g_object_ref (addr);
  slot->seen = now;
}

static void
drop_peers (LogflDupSrv *s)
{
  for (guint i = 0; i < DUP_MAX_PEERS; i++)
    g_clear_object (&s->peers[i].addr);
}

void
logfl_dup_srv_notify (LogflDupSrv *s, const char *call, LogflDupVerdict v)
{
  if (!s || !s->sock || !call || !*call)
    return;
  char *msg = NULL;
  gint64 now = g_get_monotonic_time ();
  for (guint i = 0; i < DUP_MAX_PEERS; i++)
    {
      DupPeer *p = &s->peers[i];
      if (!p->addr)
        continue;
      if (now - p->seen > DUP_PEER_TTL_US)
        {
          g_clear_object (&p->addr);
          continue;
        }
      if (!msg)
        msg = g_strdup_printf ("%s %s\n", logfl_dup_verdict_str (v), call);
      g_socket_send_to (s->sock, p->addr, msg, strlen (msg), NULL, NULL);
    }
  g_free (msg);
}

static gboolean
on_udp (GSocket *sock, GIOCondition cond, gpointer user_data)
{
  LogflDupSrv *s = user_data;
  (void) cond;
  if (!s->sock)
    return G_SOURCE_CONTINUE;

  /* Receive regardless of the condition bits. Linux reports async errors
   * (ICMP port unreachable after we replied to a client that died) on
   * unconnected UDP sockets too, and the source then wakes with G_IO_ERR
   * alone; the recv is what consumes the pending error. Skipping it would
   * leave the error armed and spin the main loop. On a spurious wake the
   * non-blocking recv just returns EWOULDBLOCK. */
  char buf[512];
  GSocketAddress *from = NULL;
  gssize n = g_socket_receive_from (sock, &from, buf, sizeof buf - 1,
                                    NULL, NULL);
  if (n <= 0)
    {
      g_clear_object (&from);
      return G_SOURCE_CONTINUE;
    }
  buf[n] = '\0';

  char *call = NULL, *mode = NULL;
  gint64 hz = 0;
  if (from && s->query_cb &&
      logfl_dup_parse_query (buf, &call, &hz, &mode))
    {
      remember_peer (s, from);   /* audience for unsolicited pushes */
      LogflDupVerdict v = s->query_cb (call, hz, mode, s->query_data);
      char *reply = g_strdup_printf ("%s %s\n",
                                     logfl_dup_verdict_str (v), call);
      g_socket_send_to (sock, from, reply, strlen (reply), NULL, NULL);
      g_free (reply);
    }
  g_free (call);
  g_free (mode);
  g_clear_object (&from);
  return G_SOURCE_CONTINUE;
}

gboolean
logfl_dup_srv_start (LogflDupSrv *s, GError **error)
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
      g_set_error (error, LOGFL_DUP_ERROR, LOGFL_DUP_ERROR_BIND,
                   "UDP socket: %s", err ? err->message : "?");
      g_clear_error (&err);
      return FALSE;
    }
  g_socket_set_blocking (sock, FALSE);

  GInetAddress *ia = g_inet_address_new_from_string (s->host);
  if (!ia)
    {
      g_object_unref (sock);
      g_set_error (error, LOGFL_DUP_ERROR, LOGFL_DUP_ERROR_BIND,
                   "bad bind host: %s", s->host);
      return FALSE;
    }
  GSocketAddress *addr = g_inet_socket_address_new (ia, s->port);
  g_object_unref (ia);
  /* No reuse — same reasoning as the WSJT-X server: SO_REUSEPORT on UDP
   * silently splits datagrams with whoever holds the port. Fail loud. */
  if (!g_socket_bind (sock, addr, FALSE, &err))
    {
      g_set_error (error, LOGFL_DUP_ERROR, LOGFL_DUP_ERROR_BIND,
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
logfl_dup_srv_stop (LogflDupSrv *s)
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
  drop_peers (s);
}
