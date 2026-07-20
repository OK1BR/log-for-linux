/* main.c — log-for-linux GTK4/libadwaita front-end bootstrap.
 *
 * The logbook engine lives in src/engine/ and stays GLib-only (no GTK) —
 * the same architecture as sdr-for-linux and skimmer-for-linux, so engine
 * code is testable headless. The window itself is win.c.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include <adwaita.h>

#include "win.h"

static void
on_activate (AdwApplication *app, gpointer user_data)
{
  (void) user_data;

  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
  if (!win)
    win = GTK_WINDOW (logfl_window_new (app));
  gtk_window_present (win);
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
