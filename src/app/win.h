/* win.h — the main logbook window (M3, docs/SCOPE.md).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_WIN_H
#define LOGFL_WIN_H

#include <adwaita.h>

G_BEGIN_DECLS

#define LOGFL_TYPE_WINDOW (logfl_window_get_type ())
G_DECLARE_FINAL_TYPE (LogflWindow, logfl_window, LOGFL, WINDOW,
                      AdwApplicationWindow)

GtkWidget *logfl_window_new (AdwApplication *app);

G_END_DECLS

#endif /* LOGFL_WIN_H */
