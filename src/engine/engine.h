/* engine.h — logbook engine identity + selfcheck.
 *
 * The engine is GLib-only (no GTK): everything under src/engine/ must build
 * and run headless so the gates (log-*-test) can exercise it under meson test.
 * M0 ships this identity module; the log store lands in M1 (docs/SCOPE.md).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_ENGINE_H
#define LOGFL_ENGINE_H

#include <glib.h>

G_BEGIN_DECLS

#define LOGFL_ENGINE_ERROR logfl_engine_error_quark ()
GQuark logfl_engine_error_quark (void);

typedef enum {
  LOGFL_ENGINE_ERROR_SQLITE,
} LogflEngineError;

/* Project version (LOGFL_VERSION at build time). */
const char *logfl_engine_version (void);

/* Version string of the sqlite3 library actually linked in. */
const char *logfl_engine_sqlite_version (void);

/* Proves the sqlite3 dependency end to end: opens an in-memory database,
 * creates a table, writes and reads a row back. TRUE on success. */
gboolean logfl_engine_selfcheck (GError **error);

G_END_DECLS

#endif /* LOGFL_ENGINE_H */
