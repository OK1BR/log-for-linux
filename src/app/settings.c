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

static const char *
bank_group (LogflMacroBankId bank)
{
  return bank == LOGFL_MACRO_BANK_SNP ? "macros_snp" : "macros_run";
}

static void
load_macros (GKeyFile *kf, LogflMacroSet *set)
{
  for (guint b = 0; b < LOGFL_MACRO_BANK_N; b++)
    {
      const char *grp = bank_group ((LogflMacroBankId) b);
      for (guint i = 0; i < LOGFL_MACRO_N_KEYS; i++)
        {
          char ck[32], tk[32];
          g_snprintf (ck, sizeof ck, "f%u_caption", i + 1);
          g_snprintf (tk, sizeof tk, "f%u_tmpl", i + 1);
          char *cap = g_key_file_get_string (kf, grp, ck, NULL);
          char *tmpl = g_key_file_get_string (kf, grp, tk, NULL);
          if (cap || tmpl)
            {
              const LogflMacroKey *cur =
                  logfl_macro_set_key (set, (LogflMacroBankId) b, i);
              logfl_macro_set_set_key (
                  set, (LogflMacroBankId) b, i,
                  cap ? cap : (cur && cur->caption ? cur->caption : ""),
                  tmpl ? tmpl : (cur && cur->tmpl ? cur->tmpl : ""));
            }
          g_free (cap);
          g_free (tmpl);
        }
    }
}

static void
save_macros (GKeyFile *kf, const LogflMacroSet *set)
{
  for (guint b = 0; b < LOGFL_MACRO_BANK_N; b++)
    {
      const char *grp = bank_group ((LogflMacroBankId) b);
      for (guint i = 0; i < LOGFL_MACRO_N_KEYS; i++)
        {
          const LogflMacroKey *k =
              logfl_macro_set_key (set, (LogflMacroBankId) b, i);
          char ck[32], tk[32];
          g_snprintf (ck, sizeof ck, "f%u_caption", i + 1);
          g_snprintf (tk, sizeof tk, "f%u_tmpl", i + 1);
          g_key_file_set_string (kf, grp, ck,
                                 k && k->caption ? k->caption : "");
          g_key_file_set_string (kf, grp, tk, k && k->tmpl ? k->tmpl : "");
        }
    }
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
  s->esm_enabled = FALSE;
  s->macro_bank = LOGFL_MACRO_BANK_RUN;
  logfl_macro_set_init_defaults (&s->macros);
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

      if (g_key_file_has_key (kf, "contest", "esm", NULL))
        s->esm_enabled = g_key_file_get_boolean (kf, "contest", "esm", NULL);

      char *bank = g_key_file_get_string (kf, "contest", "bank", NULL);
      if (bank)
        {
          if (g_ascii_strcasecmp (bank, "snp") == 0 ||
              g_ascii_strcasecmp (bank, "s&p") == 0)
            s->macro_bank = LOGFL_MACRO_BANK_SNP;
          else
            s->macro_bank = LOGFL_MACRO_BANK_RUN;
          g_free (bank);
        }

      load_macros (kf, &s->macros);
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

  g_key_file_set_boolean (kf, "contest", "esm", s->esm_enabled);
  g_key_file_set_string (kf, "contest", "bank",
                         s->macro_bank == LOGFL_MACRO_BANK_SNP ? "snp"
                                                               : "run");
  save_macros (kf, &s->macros);

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
  s->esm_enabled = FALSE;
  s->macro_bank = LOGFL_MACRO_BANK_RUN;
  logfl_macro_set_clear (&s->macros);
}
