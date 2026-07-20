project('log-for-linux', 'c',
  version : '0.0.0',
  license : 'GPL-3.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language : 'c')
add_project_arguments('-DLOGFL_VERSION="@0@"'.format(meson.project_version()),
                      language : 'c')

# --- Dependencies -----------------------------------------------------------
# Same split as sdr-for-linux / skimmer-for-linux: the logbook engine is
# GLib-only (headless, testable), the app adds GTK4/libadwaita on top.
glib_dep = dependency('glib-2.0')
gio_dep  = dependency('gio-2.0')
gtk_dep  = dependency('gtk4')
adw_dep  = dependency('libadwaita-1')

# --- App --------------------------------------------------------------------
executable('log-for-linux',
  'src/app/main.c',
  dependencies : [glib_dep, gio_dep, gtk_dep, adw_dep],
  install : true,
)
