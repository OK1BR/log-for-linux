/* adif.h — ADIF 3.1.x (.adi) import/export over the log store (M2).
 *
 * The parser is deliberately tolerant of real-world exports: tag names in any
 * case, CRLF or odd whitespace between fields, a missing <EOH>, a `:type`
 * suffix on tags, garbage between fields, a final record without <EOR> and a
 * 4-digit TIME_ON all parse. Fields the store does not model are preserved
 * verbatim in LogflQso.extras and written back on export — import → export
 * never drops data (docs/SCOPE.md).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_ADIF_H
#define LOGFL_ADIF_H

#include "log_store.h"

G_BEGIN_DECLS

typedef struct {
  guint n_imported;
  guint n_dup_skipped;   /* same call+band+mode within the dup window */
  guint n_bad;           /* records missing call, date, mode or band+freq */
} LogflAdifReport;

/* Imports records into the store inside one transaction. dup_window_s is the
 * skip window in seconds around an existing same-call+band+mode QSO
 * (0 = only an identical timestamp is a dup). Broken records are counted in
 * report->n_bad, they never abort the import; FALSE + error only on file/SQL
 * level failures (nothing is imported then). len -1 = NUL-terminated. */
gboolean logfl_adif_import_data (LogflStore *s, const char *data, gssize len,
                                 guint dup_window_s, LogflAdifReport *report,
                                 GError **error);
gboolean logfl_adif_import_file (LogflStore *s, const char *path,
                                 guint dup_window_s, LogflAdifReport *report,
                                 GError **error);

/* Exports matching QSOs (all when query is NULL) oldest-first, with a fixed
 * deterministic header and field order — exporting the same log twice yields
 * byte-identical output. Returns the ADIF text (g_free) or NULL + error. */
char *logfl_adif_export_data (LogflStore *s, const LogflStoreQuery *query,
                              guint *n_exported, GError **error);
gboolean logfl_adif_export_file (LogflStore *s, const char *path,
                                 const LogflStoreQuery *query,
                                 guint *n_exported, GError **error);

/* ADIF band name ("40m") for a frequency in MHz, NULL when outside every
 * amateur allocation the table knows. */
const char *logfl_adif_band_for_freq (double mhz);

/* Mid-point of the band allocation in MHz, or 0 if the name is unknown.
 * Fallback only — prefer the live VFO / typed MHz when logging. */
double logfl_adif_freq_for_band (const char *band);

G_END_DECLS

#endif /* LOGFL_ADIF_H */
