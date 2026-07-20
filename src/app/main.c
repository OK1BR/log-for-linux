/* main.c — log-for-linux GTK4/libadwaita front-end.
 *
 * Skeleton only: an AdwApplicationWindow shell. The logbook engine lives in
 * src/engine/ and stays GLib-only (no GTK) — the same architecture as
 * sdr-for-linux and skimmer-for-linux, so engine code is testable headless.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include <adwaita.h>

#include "engine.h"
#include "log_store.h"

static void
on_activate (AdwApplication *app, gpointer user_data)
{
  (void) user_data;

  GtkWidget *win = adw_application_window_new (GTK_APPLICATION (app));
  gtk_window_set_title (GTK_WINDOW (win), "Log for Linux");
  gtk_window_set_default_size (GTK_WINDOW (win), 900, 600);

  GtkWidget *tbv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tbv), adw_header_bar_new ());

  GError *err = NULL;
  gboolean engine_ok = logfl_engine_selfcheck (&err);
  g_autofree char *engine_line = g_strdup_printf (
      "Engine %s · SQLite %s · selfcheck %s",
      logfl_engine_version (), logfl_engine_sqlite_version (),
      engine_ok ? "OK" : err->message);
  g_clear_error (&err);

  /* M1: the real store. The UI proper lands in M3 — for now open it, show
   * the counters and close again. */
  g_autofree char *db_dir =
      g_build_filename (g_get_user_data_dir (), "log-for-linux", NULL);
  g_mkdir_with_parents (db_dir, 0700);
  g_autofree char *db_path = g_build_filename (db_dir, "log.db", NULL);
  g_autofree char *store_line = NULL;
  LogflStore *store = logfl_store_open (db_path, &err);
  if (store)
    {
      LogflStoreStats st;
      if (logfl_store_stats (store, &st, &err))
        store_line = g_strdup_printf (
            "Log: %u QSO · %u calls (schema v%d, %s)",
            st.n_qso, st.n_calls, LOGFL_STORE_SCHEMA_VERSION, db_path);
      logfl_store_close (store);
    }
  if (!store_line)
    {
      store_line = g_strdup_printf ("Log store FAILED: %s", err->message);
      g_clear_error (&err);
    }

  g_autofree char *desc = g_strdup_printf (
      "Native ham radio logbook — M1 (log store), UI lands in M3.\n%s\n%s",
      engine_line, store_line);

  GtkWidget *status = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (status), "Log for Linux");
  adw_status_page_set_description (ADW_STATUS_PAGE (status), desc);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tbv), status);

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (win), tbv);
  gtk_window_present (GTK_WINDOW (win));
}

int
main (int argc, char **argv)
{
  AdwApplication *app =
      adw_application_new ("cz.ok1br.log_for_linux", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  int status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
