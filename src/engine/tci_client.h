/* tci_client.h — TCI (ExpertSDR) WebSocket client for the logbook (M4).
 *
 * Connects to sdr-for-linux — over its Unix socket when it is on this
 * machine, over TCP otherwise (#32) — completes the handshake (ready;),
 * tracks VFO/mode/device, and can QSY on an explicit user action
 * (double-click a logged QSO). Text plane only — no IQ/audio streams
 * (unlike skimmer-for-linux). Template: skimmer tci_client.c.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_TCI_CLIENT_H
#define LOGFL_TCI_CLIENT_H

#include <glib.h>

G_BEGIN_DECLS

#define LOGFL_TCI_DEFAULT_PORT 40001
#define LOGFL_TCI_DEFAULT_TCP  "ws://127.0.0.1:40001"

/* Keyer speed limits — mirror sdr-for-linux tci_set_cw_speed() so the local
 * (optimistic) value can never drift outside what the radio will accept. */
#define LOGFL_TCI_WPM_MIN 5
#define LOGFL_TCI_WPM_MAX 60

typedef struct _LogflTciClient LogflTciClient;

/* ---- server address ------------------------------------------------------
 *
 * One string names the server, in one of three spellings:
 *   unix:/path        — the sdr-for-linux Unix socket (absolute path)
 *   ws://host[:port]  — TCP, port 40001 when omitted
 *   host[:port]       — the same, the way the old Host field was typed
 * An empty string means the default order: the local socket at
 * $XDG_RUNTIME_DIR/sdr-for-linux/tci.sock first, then ws://127.0.0.1:40001 —
 * a fresh install works whichever listener the radio has on, and a radio on
 * another machine just gets its TCP address. */

/* sockaddr_un.sun_path on Linux — a longer path cannot be dialled. */
#define LOGFL_TCI_SOCK_PATH_MAX 108

typedef struct {
  gboolean unix_sock;                       /* TRUE: path; FALSE: host/port */
  char     path[LOGFL_TCI_SOCK_PATH_MAX];
  char     host[256];
  guint16  port;
} LogflTciEndpoint;

/* Parse one spelling above into *out. FALSE (out untouched) for an empty
 * or malformed string — a relative socket path, wss://, a bad port. */
gboolean logfl_tci_endpoint_parse (const char *s, LogflTciEndpoint *out);

/* Canonical spelling, "unix:/path" or "ws://host:port"; caller frees. */
char *logfl_tci_endpoint_to_string (const LogflTciEndpoint *ep);

/* $XDG_RUNTIME_DIR/sdr-for-linux/tci.sock — where sdr-for-linux listens
 * unless its own preference moved the socket; caller frees. */
char *logfl_tci_default_socket_path (void);

/* ---- state ------------------------------------------------------------- */

/* Snapshot of the last known radio state. mode is the TCI string
 * (lowercase: "cw", "usb", …). Valid after a successful start().
 * cw_wpm is 0 until the radio reports cw_macros_speed. */
typedef struct {
  double vfo_hz;
  char   mode[32];
  char   device[64];
  char   protocol[64];
  int    cw_wpm;
} LogflTciState;

/* State changed (vfo and/or modulation). Fires on the LWS service thread —
 * consumers must marshal to the GLib main loop themselves. */
typedef void (*LogflTciStateCb) (const LogflTciState *st, gpointer user_data);

/* Unexpected disconnect while running. Does NOT fire on stop(). LWS thread. */
typedef void (*LogflTciClosedCb) (gpointer user_data);

/* The operator clicked a spot on the radio's panadapter — sdr-for-linux
 * broadcasts the callsign it just tuned to (skimmer-for-linux feeds those
 * spots in over its own TCI link). An event, not state: it fires once per
 * click, on the LWS thread. call is upper-case and validated as plausible;
 * freq_hz is the spot's exact frequency. */
typedef void (*LogflTciSpotCb) (const char *call, double freq_hz,
                                gpointer user_data);

/* server: one address in a spelling above, or NULL/"" for the default
 * order. A malformed string is kept and reported by start(). */
LogflTciClient *logfl_tci_client_new (const char *server);

/* An explicit try-order (NULL-terminated): start() takes the first server
 * that answers ready;. The default order comes through here; the gate uses
 * it to run the fallback against its own mock instead of the live port. */
LogflTciClient *logfl_tci_client_new_order (const char *const *servers);

void            logfl_tci_client_free (LogflTciClient *c);

void logfl_tci_client_set_state_cb  (LogflTciClient *c, LogflTciStateCb cb,
                                     gpointer user_data);
void logfl_tci_client_set_closed_cb (LogflTciClient *c, LogflTciClosedCb cb,
                                     gpointer user_data);
void logfl_tci_client_set_spot_cb   (LogflTciClient *c, LogflTciSpotCb cb,
                                     gpointer user_data);

/* Connect and wait for ready; (blocks up to a few seconds per candidate).
 * No IQ start. */
gboolean logfl_tci_client_start (LogflTciClient *c, GError **error);
void     logfl_tci_client_stop  (LogflTciClient *c);

gboolean logfl_tci_client_is_ready (LogflTciClient *c);
void logfl_tci_client_get_state (LogflTciClient *c, LogflTciState *out);

/* The address the client is connected over, in the canonical spelling —
 * which of the candidates won. NULL when not started; owned by the client,
 * valid until the next start()/stop(). */
const char *logfl_tci_client_endpoint (LogflTciClient *c);

/* Explicit user QSY only — never called from automatic state updates.
 * No UI calls it today: table-driven QSY was tried and dropped
 * (2026-07-21) — a click on a table cell is reserved for inline edit. */
void logfl_tci_client_tune (LogflTciClient *c, double freq_hz);

/* CW keyer via TCI (sdr-for-linux cw_macros / cw_macros_stop). Text must not
 * contain ':' or ';' — they are scrubbed. No-op when not connected. */
void logfl_tci_client_cw_send (LogflTciClient *c, const char *text);
void logfl_tci_client_cw_stop (LogflTciClient *c);

/* RTTY keying via TCI (rtty_macros / rtty_macros_stop — family extension,
 * radio side sdr-for-linux ≥ f38aafd; older servers ignore the command).
 * Same scrub and leading-space semantics as the CW pair. */
void logfl_tci_client_rtty_send (LogflTciClient *c, const char *text);
void logfl_tci_client_rtty_stop (LogflTciClient *c);

/* Keyer speed (Page Up/Down in the contest UI). Clamped to WPM_MIN..MAX and
 * sent as cw_macros_speed:<wpm>; the radio echoes the accepted value back,
 * which updates the state snapshot. No-op when not connected. */
void logfl_tci_client_cw_set_speed (LogflTciClient *c, int wpm);

/* Last speed reported by the radio, or 0 when not known yet. */
int  logfl_tci_client_cw_speed (LogflTciClient *c);

/* Map a TCI modulation string to a logbook mode dropdown value, or NULL
 * when the mapping is unknown (leave the UI alone). */
const char *logfl_tci_mode_to_log (const char *tci_mode);

G_END_DECLS

#endif /* LOGFL_TCI_CLIENT_H */
