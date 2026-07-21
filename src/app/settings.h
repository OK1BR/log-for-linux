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

G_BEGIN_DECLS

typedef struct {
  char   *tci_host;            /* sdr-for-linux WebSocket host               */
  guint16 tci_port;            /* default 40001                              */
  char   *station_callsign;    /* STATION_CALLSIGN stamped on new QSOs       */
  gboolean esm_enabled;        /* M5: Enter advances ESM instead of log      */
  LogflMacroBankId macro_bank; /* Run or S&P                                 */
  LogflMacroSet macros;        /* F1–F12 + free row + STOP per bank          */
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
