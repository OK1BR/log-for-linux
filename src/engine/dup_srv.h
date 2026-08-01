/* dup_srv.h — worked/dup lookup service for sibling apps (skimmer).
 *
 * Tiny read-only UDP question/answer on localhost: a client (skimmer-for-
 * linux) asks about a callsign it is about to spot, the logbook answers
 * from its store so the client can color the spot / decode highlight and
 * the operator does not click duplicates.
 *
 *   request:  "DUP? <call> <freq_hz> <mode>"     (single datagram, UTF-8)
 *   reply:    "NEW <call>" | "B4 <call>" | "DUP <call>"   (to the sender)
 *
 * DUP = duplicate under the ACTIVE CONTEST rule (same call+band+mode in
 * this contest), B4 = worked before ever (any time, any contest), NEW =
 * never in the log. Malformed requests get no reply — the client treats
 * a timeout as "unknown" and keeps its default color.
 *
 * The service is transport only: parsing + reply formatting live here
 * (GLib/Gio, headless gate log-dupq-test); the verdict comes from an app
 * callback, which owns store access and the contest context.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_DUP_SRV_H
#define LOGFL_DUP_SRV_H

#include <glib.h>

G_BEGIN_DECLS

#define LOGFL_DUP_ERROR logfl_dup_error_quark ()
GQuark logfl_dup_error_quark (void);

typedef enum {
  LOGFL_DUP_ERROR_BIND,
} LogflDupError;

/* Localhost only by design — the log's worked list is nobody's business
 * off this machine. 2238 = one above the WSJT-X default the app already
 * listens on (2237). */
#define LOGFL_DUP_DEFAULT_HOST "127.0.0.1"
#define LOGFL_DUP_DEFAULT_PORT 2238

typedef enum {
  LOGFL_DUP_NEW = 0,
  LOGFL_DUP_B4,
  LOGFL_DUP_DUP,
} LogflDupVerdict;

typedef struct _LogflDupSrv LogflDupSrv;

/* Answer a parsed query. Runs on the context that owns the socket source
 * (usually main), so the app may touch its store directly. */
typedef LogflDupVerdict (*LogflDupQueryCb) (const char *call, gint64 freq_hz,
                                            const char *mode,
                                            gpointer user_data);

LogflDupSrv *logfl_dup_srv_new  (const char *bind_host, guint16 port);
void         logfl_dup_srv_free (LogflDupSrv *s);

void logfl_dup_srv_set_query_cb (LogflDupSrv *s, LogflDupQueryCb cb,
                                 gpointer user_data);

/* Bind UDP + attach a GSource. port 0 = ephemeral (gate test);
 * logfl_dup_srv_port then reports the bound one. */
gboolean logfl_dup_srv_start (LogflDupSrv *s, GError **error);
void     logfl_dup_srv_stop  (LogflDupSrv *s);
gboolean logfl_dup_srv_is_running (const LogflDupSrv *s);
guint16  logfl_dup_srv_port (const LogflDupSrv *s);

/* --- exposed for the gate test ------------------------------------------ */

/* Parse "DUP? <call> <freq_hz> <mode>". Out strings are owned by the
 * caller. FALSE on anything malformed (wrong verb, missing fields,
 * hz <= 0). */
gboolean logfl_dup_parse_query (const char *line, char **call, gint64 *hz,
                                char **mode);

const char *logfl_dup_verdict_str (LogflDupVerdict v);

G_END_DECLS

#endif /* LOGFL_DUP_SRV_H */
