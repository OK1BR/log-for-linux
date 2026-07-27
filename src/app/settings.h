/* settings.h — persistent app prefs (GKeyFile INI).
 *
 * Path: $XDG_CONFIG_HOME/log-for-linux/settings.ini
 * Same house style as skimmer-for-linux (settings.ini + AdwPreferencesDialog)
 * and sdr-for-linux (dedicated settings module over GKeyFile — not GSettings).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_SETTINGS_H
#define LOGFL_SETTINGS_H

#include <glib.h>

#include "macros.h"
#include "wsjtx_udp.h"

G_BEGIN_DECLS

typedef struct {
  char   *tci_host;            /* sdr-for-linux WebSocket host               */
  guint16 tci_port;            /* default 40001                              */
  char   *station_callsign;    /* STATION_CALLSIGN stamped on new QSOs       */
  char   *station_grid;        /* my locator → MY_GRIDSQUARE on new QSOs     */
  guint   station_cqz;         /* my CQ (DX) zone, 0 = unset (OK = 15)       */
  guint   station_ituz;        /* my ITU zone, 0 = unset (OK = 28)           */
  gboolean esm_enabled;        /* M5: Enter advances ESM instead of log      */
  LogflMacroBankId macro_bank; /* Run or S&P                                 */
  LogflMacroSet macros;        /* F1–F8 + free row + STOP (16) per bank      */
  gboolean wsjtx_enabled;      /* M6: listen for WSJT-X / JTDX UDP           */
  guint16  wsjtx_port;         /* default 2237                               */
  gint64   active_contest;     /* contest.id the UI is switched to; 0 = main */
  /* Cabrillo export header prefill ([cabrillo] in the ini). */
  char *cab_operator, *cab_band, *cab_mode, *cab_power, *cab_transmitter;
  char *cab_assisted;          /* "" = omit the tag */
  char *cab_name, *cab_email, *cab_location, *cab_club;
} LogflSettings;

/* Fill defaults (heap strings + macro banks). Safe on a zeroed struct. */
void logfl_settings_init_defaults (LogflSettings *s);

/* Load from disk, keeping defaults for missing keys. */
void logfl_settings_load (LogflSettings *s);

/* Atomic-ish write (mkdir parents, overwrite file). */
void logfl_settings_save (const LogflSettings *s);

/* Free heap fields; leaves the struct zeroed. */
void logfl_settings_clear (LogflSettings *s);

G_END_DECLS

#endif /* LOGFL_SETTINGS_H */
