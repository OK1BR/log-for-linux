/* tci_client.h — TCI (ExpertSDR) WebSocket client for the logbook (M4).
 *
 * Connects to sdr-for-linux (ws://host:40001 by default), completes the
 * handshake (ready;), tracks VFO/mode/device, and can QSY on an explicit
 * user action (double-click a logged QSO). Text plane only — no IQ/audio
 * streams (unlike skimmer-for-linux). Template: skimmer tci_client.c.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_TCI_CLIENT_H
#define LOGFL_TCI_CLIENT_H

#include <glib.h>

G_BEGIN_DECLS

#define LOGFL_TCI_DEFAULT_HOST "127.0.0.1"
#define LOGFL_TCI_DEFAULT_PORT 40001

/* Keyer speed limits — mirror sdr-for-linux tci_set_cw_speed() so the local
 * (optimistic) value can never drift outside what the radio will accept. */
#define LOGFL_TCI_WPM_MIN 5
#define LOGFL_TCI_WPM_MAX 60

typedef struct _LogflTciClient LogflTciClient;

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

LogflTciClient *logfl_tci_client_new  (const char *host, guint16 port);
void            logfl_tci_client_free (LogflTciClient *c);

void logfl_tci_client_set_state_cb  (LogflTciClient *c, LogflTciStateCb cb,
                                     gpointer user_data);
void logfl_tci_client_set_closed_cb (LogflTciClient *c, LogflTciClosedCb cb,
                                     gpointer user_data);
void logfl_tci_client_set_spot_cb   (LogflTciClient *c, LogflTciSpotCb cb,
                                     gpointer user_data);

/* Connect and wait for ready; (blocks up to a few seconds). No IQ start. */
gboolean logfl_tci_client_start (LogflTciClient *c, GError **error);
void     logfl_tci_client_stop  (LogflTciClient *c);

gboolean logfl_tci_client_is_ready (LogflTciClient *c);
void     logfl_tci_client_get_state (LogflTciClient *c, LogflTciState *out);

/* Explicit user QSY only — never called from automatic state updates. */
void logfl_tci_client_tune (LogflTciClient *c, double freq_hz);

/* CW keyer via TCI (sdr-for-linux cw_macros / cw_macros_stop). Text must not
 * contain ':' or ';' — they are scrubbed. No-op when not connected. */
void logfl_tci_client_cw_send (LogflTciClient *c, const char *text);
void logfl_tci_client_cw_stop (LogflTciClient *c);

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
