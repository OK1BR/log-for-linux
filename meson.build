project('log-for-linux', 'c',
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language : 'c')

gnome = import('gnome')  # used in data/ for post-install icon/desktop DB refresh
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
  'src/engine/wsjtx_udp.c',    # M6: WSJT-X / JTDX UDP (QSO Logged, highlight)
  'src/engine/dup_srv.c',      # worked/dup lookup UDP service (skimmer asks)
  'src/engine/contest.c',      # M9: contest exchange templates + presets
  'src/engine/cabrillo.c',     # M9: Cabrillo v3 contest log export
  'src/engine/cty.c',          # country/continent from a call (cty.dat)
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

# M6: WSJT-X UDP — pack/parse QSO Logged, loopback server, highlight.
log_udp_test = executable('log-udp-test',
  ['src/udp_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('udp', log_udp_test, timeout : 30)

# Worked/dup lookup service — request parser + loopback round trip.
log_dupq_test = executable('log-dupq-test',
  ['src/dupq_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('dupq', log_dupq_test, timeout : 30)

# M9: contests — exchange templates, v1→v2 migration, scoping, delete, ADIF.
log_contest_test = executable('log-contest-test',
  ['src/contest_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('contest', log_contest_test, timeout : 30)

# M9: Cabrillo — header tags, QSO line shape, ordering, freq/mode mapping.
log_cabrillo_test = executable('log-cabrillo-test',
  ['src/cabrillo_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('cabrillo', log_cabrillo_test, timeout : 30)

# cty.dat parser — format corners on a synthetic file, then the bundled
# snapshot must resolve real calls (OK/OL, OH, K, PY, …).
log_cty_test = executable('log-cty-test',
  ['src/cty_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('cty', log_cty_test, timeout : 30,
  args : [meson.current_source_dir() / 'data' / 'cty.dat'])

# --- App --------------------------------------------------------------------
executable('log-for-linux',
  [
    'src/app/main.c',
    'src/app/win.c',        # M3 UI + M4 TCI + M5 macros + M6 WSJT-X
    'src/app/qso_row.c',    # M3: GObject row wrapper for GtkColumnView
    'src/app/settings.c',   # GKeyFile prefs (TCI, station, macros, ESM, WSJT-X)
  ],
  include_directories : engine_inc,
  dependencies : [glib_dep, gio_dep, gtk_dep, adw_dep, lws_dep],
  link_with : engine_lib,
  install : true,
)

subdir('data')   # .desktop + icon + AppStream metainfo (install only)
