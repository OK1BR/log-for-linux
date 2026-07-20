/* main.c — log-for-linux GTK4/libadwaita front-end.
 *
 * Skeleton only: an AdwApplicationWindow shell. The logbook engine lives in
 * src/engine/ and stays GLib-only (no GTK) — the same architecture as
 * sdr-for-linux and skimmer-for-linux, so engine code is testable headless.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include <adwaita.h>

#ifndef LOGFL_VERSION
#define LOGFL_VERSION "0.0.0"
#endif

static void
on_activate (AdwApplication *app, gpointer user_data)
{
  (void) user_data;

  GtkWidget *win = adw_application_window_new (GTK_APPLICATION (app));
  gtk_window_set_title (GTK_WINDOW (win), "Log for Linux");
  gtk_window_set_default_size (GTK_WINDOW (win), 900, 600);

  GtkWidget *tbv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tbv), adw_header_bar_new ());

  GtkWidget *status = adw_status_page_new ();
  adw_status_page_set_title (ADW_STATUS_PAGE (status), "Log for Linux");
  adw_status_page_set_description (ADW_STATUS_PAGE (status),
      "Native ham radio logbook — project skeleton, nothing logged yet.");
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
