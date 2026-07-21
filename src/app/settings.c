/* settings.c — GKeyFile persistence for log-for-linux prefs.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "settings.h"

#include "tci_client.h"

#include <string.h>

static char *
settings_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "log-for-linux",
                           "settings.ini", NULL);
}

void
logfl_settings_init_defaults (LogflSettings *s)
{
  g_return_if_fail (s != NULL);
  g_free (s->tci_host);
  g_free (s->station_callsign);
  s->tci_host = g_strdup (LOGFL_TCI_DEFAULT_HOST);
  s->tci_port = LOGFL_TCI_DEFAULT_PORT;
  s->station_callsign = g_strdup ("OK1BR");
}

void
logfl_settings_load (LogflSettings *s)
{
  g_return_if_fail (s != NULL);
  logfl_settings_init_defaults (s);

  char *path = settings_path ();
  GKeyFile *kf = g_key_file_new ();
  if (g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL))
    {
      char *host = g_key_file_get_string (kf, "tci", "host", NULL);
      if (host && *host)
        {
          g_free (s->tci_host);
          s->tci_host = g_strstrip (host);
        }
      else
        g_free (host);

      if (g_key_file_has_key (kf, "tci", "port", NULL))
        {
          int p = g_key_file_get_integer (kf, "tci", "port", NULL);
          if (p > 0 && p <= 65535)
            s->tci_port = (guint16) p;
        }

      char *call = g_key_file_get_string (kf, "station", "callsign", NULL);
      if (call && *call)
        {
          g_free (s->station_callsign);
          s->station_callsign = g_strstrip (call);
        }
      else
        g_free (call);
    }
  g_key_file_free (kf);
  g_free (path);
}

void
logfl_settings_save (const LogflSettings *s)
{
  g_return_if_fail (s != NULL);
  char *path = settings_path ();
  char *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0755);

  GKeyFile *kf = g_key_file_new ();
  /* Preserve unknown groups/keys from future versions or hand edits. */
  g_key_file_load_from_file (kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);

  g_key_file_set_string (kf, "tci", "host",
                         s->tci_host && *s->tci_host
                             ? s->tci_host
                             : LOGFL_TCI_DEFAULT_HOST);
  g_key_file_set_integer (kf, "tci", "port",
                          s->tci_port ? s->tci_port : LOGFL_TCI_DEFAULT_PORT);
  g_key_file_set_string (kf, "station", "callsign",
                         s->station_callsign ? s->station_callsign : "");

  GError *err = NULL;
  if (!g_key_file_save_to_file (kf, path, &err))
    {
      g_warning ("settings: %s not saved: %s", path,
                 err ? err->message : "?");
      g_clear_error (&err);
    }
  g_key_file_free (kf);
  g_free (dir);
  g_free (path);
}

void
logfl_settings_clear (LogflSettings *s)
{
  if (!s)
    return;
  g_clear_pointer (&s->tci_host, g_free);
  g_clear_pointer (&s->station_callsign, g_free);
  s->tci_port = 0;
}
