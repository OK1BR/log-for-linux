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
glib_dep   = dependency('glib-2.0')
gio_dep    = dependency('gio-2.0')             # M4/M5: TCI + WSJT-X sockets
gtk_dep    = dependency('gtk4')                # app front-end
adw_dep    = dependency('libadwaita-1')        # app control surface
sqlite_dep = dependency('sqlite3')             # M1: canonical log store
lws_dep    = dependency('libwebsockets')       # M4: TCI client transport

engine_inc = include_directories('src/engine')

# --- Engine (headless, GLib-only) -------------------------------------------
# Grows milestone by milestone (docs/SCOPE.md). M0: identity + sqlite selfcheck.
engine_sources = [
  'src/engine/engine.c',       # M0: identity, sqlite selfcheck
  'src/engine/log_store.c',    # M1: canonical QSO store (schema, CRUD, queries)
  'src/engine/adif.c',         # M2: ADIF import/export, lossless round-trip
  'src/engine/tci_client.c',   # M4: TCI client (VFO/mode, explicit QSY)
  'src/engine/macros.c',       # M5: F-key expand, Run/S&P banks, ESM
]
engine_deps = [glib_dep, gio_dep, sqlite_dep, lws_dep]

engine_lib = static_library('logfl-engine',
  engine_sources,
  include_directories : engine_inc,
  dependencies : engine_deps,
)

# --- Gates (offline tests, family house style) -------------------------------
# M0: the engine builds headless and the sqlite dependency works end to end.
log_engine_test = executable('log-engine-test',
  ['src/engine_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('engine', log_engine_test, timeout : 30)

# M1: log store — migrations, CRUD round-trip, dup/worked-B4, 50k volume.
log_store_test = executable('log-store-test',
  ['src/store_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('store', log_store_test, timeout : 60)

# M2: ADIF — quirks corpus, unknown-field preservation, byte-stable round trip.
log_adif_test = executable('log-adif-test',
  ['src/adif_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('adif', log_adif_test, timeout : 60)

# M4: TCI client — mock server handshake, live VFO/mode, tune().
log_tci_test = executable('log-tci-test',
  ['src/tci_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('tci', log_tci_test, timeout : 30)

# M5: macros — token expand, Run/S&P banks, ESM state transitions.
log_macro_test = executable('log-macro-test',
  ['src/macro_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('macros', log_macro_test, timeout : 30)

# --- App --------------------------------------------------------------------
executable('log-for-linux',
  [
    'src/app/main.c',
    'src/app/win.c',        # M3 UI + M4 TCI + M5 macros v2
    'src/app/qso_row.c',    # M3: GObject row wrapper for GtkColumnView
    'src/app/settings.c',   # GKeyFile prefs (TCI, station, macros, ESM)
  ],
  include_directories : engine_inc,
  dependencies : [glib_dep, gio_dep, gtk_dep, adw_dep, lws_dep],
  link_with : engine_lib,
  install : true,
)
