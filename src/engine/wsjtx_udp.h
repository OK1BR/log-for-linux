/* wsjtx_udp.h — WSJT-X / JTDX UDP protocol (M6).
 *
 * Binary QDataStream (big-endian, schema 2/3). utf8 fields are QByteArray
 * (quint32 length + bytes; 0xffffffff = null), not UTF-16 QString — see
 * NetworkMessage.hpp in WSJT-X.
 *
 * Server listens for QSO Logged → LogflQso, tracks Status for worked-B4
 * Highlight Callsign replies. GLib/Gio only; headless gate: log-udp-test.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_WSJTX_UDP_H
#define LOGFL_WSJTX_UDP_H

#include "log_store.h"

#include <glib.h>

G_BEGIN_DECLS

#define LOGFL_WSJTX_ERROR logfl_wsjtx_error_quark ()
GQuark logfl_wsjtx_error_quark (void);

typedef enum {
  LOGFL_WSJTX_ERROR_PARSE,
  LOGFL_WSJTX_ERROR_BIND,
  LOGFL_WSJTX_ERROR_IO,
} LogflWsjtxError;

#define LOGFL_WSJTX_MAGIC        0xadbccbdaU
#define LOGFL_WSJTX_SCHEMA       3
#define LOGFL_WSJTX_DEFAULT_PORT 2237
#define LOGFL_WSJTX_DEFAULT_HOST "127.0.0.1"

typedef enum {
  LOGFL_WSJTX_MSG_HEARTBEAT   = 0,
  LOGFL_WSJTX_MSG_STATUS      = 1,
  LOGFL_WSJTX_MSG_DECODE      = 2,
  LOGFL_WSJTX_MSG_CLEAR       = 3,
  LOGFL_WSJTX_MSG_REPLY       = 4,
  LOGFL_WSJTX_MSG_QSO_LOGGED  = 5,
  LOGFL_WSJTX_MSG_CLOSE       = 6,
  LOGFL_WSJTX_MSG_REPLAY      = 7,
  LOGFL_WSJTX_MSG_HALT_TX     = 8,
  LOGFL_WSJTX_MSG_FREE_TEXT   = 9,
  LOGFL_WSJTX_MSG_WSPR_DECODE = 10,
  LOGFL_WSJTX_MSG_LOCATION    = 11,
  LOGFL_WSJTX_MSG_LOGGED_ADIF = 12,
  LOGFL_WSJTX_MSG_HIGHLIGHT   = 13,
} LogflWsjtxMsgType;

typedef struct {
  guint32           magic;
  guint32           schema;
  LogflWsjtxMsgType type;
  char             *id; /* client id, e.g. "WSJT-X" — owned */
} LogflWsjtxHeader;

/* Subset of Status (type 1) used for worked-B4 highlight. */
typedef struct {
  LogflWsjtxHeader hdr;
  guint64          dial_hz;
  char            *mode;
  char            *dx_call;
  char            *dx_grid;
  char            *de_call;
  char            *de_grid;
  char            *report;
  char            *tx_mode;
} LogflWsjtxStatus;

/* QSO Logged (type 5). */
typedef struct {
  LogflWsjtxHeader hdr;
  gint64           ts_off; /* unix UTC seconds; 0 if unset */
  gint64           ts_on;
  char            *dx_call;
  char            *dx_grid;
  guint64          freq_hz;
  char            *mode;
  char            *rst_sent;
  char            *rst_rcvd;
  char            *tx_power; /* raw utf8 from WSJT-X ("5", "5W", …) */
  char            *comment;
  char            *name;
  char            *op_call;
  char            *my_call;
  char            *my_grid;
  char            *exch_sent;
  char            *exch_rcvd;
  char            *prop_mode;
} LogflWsjtxQsoLogged;

/* Decode (type 2): one decoded message from the client's Band Activity. */
typedef struct {
  LogflWsjtxHeader hdr;
  gboolean         is_new;
  guint32          time_ms;  /* QTime, ms since midnight UTC */
  gint32           snr;
  double           dt;
  guint32          df;
  char            *mode;     /* jt9 mode letter: "~" FT8, "+" FT4 … */
  char            *message;
  gboolean         low_confidence;
  gboolean         off_air;
} LogflWsjtxDecode;

void logfl_wsjtx_header_clear (LogflWsjtxHeader *h);
void logfl_wsjtx_decode_clear (LogflWsjtxDecode *d);
void logfl_wsjtx_status_clear (LogflWsjtxStatus *s);
void logfl_wsjtx_qso_logged_clear (LogflWsjtxQsoLogged *q);

/* Header only (magic/schema/type/id). *payload_off is byte offset of the
 * type-specific body (after id). */
gboolean logfl_wsjtx_parse_header (const guint8 *data, gsize len,
                                   LogflWsjtxHeader *out, gsize *payload_off,
                                   GError **error);

/* Full type-5 parse from a complete datagram. */
gboolean logfl_wsjtx_parse_qso_logged (const guint8 *data, gsize len,
                                       LogflWsjtxQsoLogged *out,
                                       GError **error);

/* Full type-2 parse. */
gboolean logfl_wsjtx_parse_decode (const guint8 *data, gsize len,
                                   LogflWsjtxDecode *out, GError **error);

/* The station that sent a standard WSJT-X message, newly allocated, or
 * NULL when the text has no usable callsign (free text, contest exchanges
 * with ';'…).  WSJT-X decodedtext.cpp tokens_re: "CQ [DX|AAAA|nnn] CALL …",
 * "CALL RR73; …" prefix, otherwise "TO FROM …"; <…> hashed calls unwrapped. */
char *logfl_wsjtx_message_de_call (const char *message);

/* Full type-1 parse (required fields + common optional tail ignored). */
gboolean logfl_wsjtx_parse_status (const guint8 *data, gsize len,
                                   LogflWsjtxStatus *out, GError **error);

/* Map QSO Logged → store QSO (owned). Band from freq; power parsed when
 * numeric. NULL if dx_call empty. */
LogflQso *logfl_wsjtx_to_qso (const LogflWsjtxQsoLogged *m);

/* --- packet builders (tests + outbound Highlight / Heartbeat) ---------- */

/* Append big-endian writers; return owned GByteArray. */
GByteArray *logfl_wsjtx_build_heartbeat (const char *id, guint32 max_schema,
                                         const char *version,
                                         const char *revision);

/* Decode for tests / replay. */
GByteArray *logfl_wsjtx_build_decode (const char *id, gboolean is_new,
                                      guint32 time_ms, gint32 snr, double dt,
                                      guint32 df, const char *mode,
                                      const char *message, gboolean low_conf);

/* Minimal Status for tests (dial_hz, mode, dx_call, de_call). */
GByteArray *logfl_wsjtx_build_status (const char *id, guint64 dial_hz,
                                      const char *mode, const char *dx_call,
                                      const char *de_call);

/* QSO Logged for tests / replay. ts_* are unix UTC; timespec UTC. */
GByteArray *logfl_wsjtx_build_qso_logged (
    const char *id, gint64 ts_on, gint64 ts_off, const char *dx_call,
    const char *dx_grid, guint64 freq_hz, const char *mode,
    const char *rst_sent, const char *rst_rcvd, const char *tx_power,
    const char *comment, const char *name, const char *my_call,
    const char *my_grid);

/* Highlight Callsign (type 13) → WSJT-X. Invalid color clears. RGB 0–255. */
GByteArray *logfl_wsjtx_build_highlight (const char *id, const char *call,
                                         gboolean bg_valid, guint8 bg_r,
                                         guint8 bg_g, guint8 bg_b,
                                         gboolean fg_valid, guint8 fg_r,
                                         guint8 fg_g, guint8 fg_b,
                                         gboolean last_only);

/* --- UDP server (GMainContext / default loop) -------------------------- */

typedef struct _LogflWsjtxServer LogflWsjtxServer;

/* Fired on the context that owns the socket source (usually main).
 * q is a newly allocated LogflQso — callback owns it (must free or store). */
typedef void (*LogflWsjtxLoggedCb) (LogflQso *q, const LogflWsjtxQsoLogged *raw,
                                    gpointer user_data);

/* Status snapshot for worked-B4 highlight decisions. */
typedef void (*LogflWsjtxStatusCb) (const LogflWsjtxStatus *st,
                                    gpointer user_data);
/* Every decode the client prints (FTX for Linux and WSJT-X send one per
 * line) — the worked-before verdict for the whole Band Activity. */
typedef void (*LogflWsjtxDecodeCb) (const LogflWsjtxDecode *d,
                                    gpointer user_data);

LogflWsjtxServer *logfl_wsjtx_server_new  (const char *bind_host,
                                           guint16 port);
void              logfl_wsjtx_server_free (LogflWsjtxServer *s);

void logfl_wsjtx_server_set_logged_cb (LogflWsjtxServer *s,
                                       LogflWsjtxLoggedCb cb,
                                       gpointer user_data);
void logfl_wsjtx_server_set_status_cb (LogflWsjtxServer *s,
                                       LogflWsjtxStatusCb cb,
                                       gpointer user_data);
void logfl_wsjtx_server_set_decode_cb (LogflWsjtxServer *s,
                                       LogflWsjtxDecodeCb cb,
                                       gpointer user_data);
/* The last Status seen: the client's dial and mode, which a Decode message
 * does not carry (0 / NULL before the first Status). */
guint64     logfl_wsjtx_server_last_dial_hz (const LogflWsjtxServer *s);
const char *logfl_wsjtx_server_last_mode (const LogflWsjtxServer *s);

/* Bind UDP and attach a GSource. FALSE + error on bind failure. */
gboolean logfl_wsjtx_server_start (LogflWsjtxServer *s, GError **error);
void     logfl_wsjtx_server_stop  (LogflWsjtxServer *s);
gboolean logfl_wsjtx_server_is_running (const LogflWsjtxServer *s);

guint16 logfl_wsjtx_server_port (const LogflWsjtxServer *s);

/* Unicast Highlight to the last peer that sent us a datagram (and matching
 * client id when non-NULL). No-op if no peer yet. */
gboolean logfl_wsjtx_server_send (LogflWsjtxServer *s, const guint8 *data,
                                  gsize len, GError **error);

/* Worked on this band and mode → yellow Highlight to the last peer; not
 * worked → nothing (the client's default colours already say "new"; one
 * reply per worked decode keeps the traffic down). */
gboolean logfl_wsjtx_server_highlight_worked (LogflWsjtxServer *s,
                                              const char *client_id,
                                              const char *call,
                                              gboolean worked, GError **error);

/* Convenience: highlight call on last peer. bg yellow when worked, green when
 * new (n_total==0); invalid bg clears. */
gboolean logfl_wsjtx_server_highlight_b4 (LogflWsjtxServer *s,
                                          const char *client_id,
                                          const char *call, guint n_total,
                                          GError **error);

G_END_DECLS

#endif /* LOGFL_WSJTX_UDP_H */
