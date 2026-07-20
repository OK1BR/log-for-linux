/* log_store.h — the canonical QSO store (M1, docs/SCOPE.md).
 *
 * SQLite-backed: one file under ~/.local/share/log-for-linux/, WAL mode,
 * schema versioned via PRAGMA user_version. First-class columns carry the
 * fields the app queries (ADIF-aligned names); everything ADIF the app does
 * not model rides in `extras` verbatim so import → export never drops data
 * (the parser/writer lands in M2).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_LOG_STORE_H
#define LOGFL_LOG_STORE_H

#include <glib.h>

G_BEGIN_DECLS

#define LOGFL_STORE_ERROR logfl_store_error_quark ()
GQuark logfl_store_error_quark (void);

typedef enum {
  LOGFL_STORE_ERROR_OPEN,
  LOGFL_STORE_ERROR_SQL,
  LOGFL_STORE_ERROR_SCHEMA,    /* file written by a NEWER app version */
  LOGFL_STORE_ERROR_NOT_FOUND,
  LOGFL_STORE_ERROR_INVALID,   /* missing call/band/mode/ts */
} LogflStoreError;

#define LOGFL_STORE_SCHEMA_VERSION 1

typedef struct _LogflStore LogflStore;

/* One QSO. Strings are owned (g_malloc'd); NULL = unset. On add/update the
 * store normalizes in place: call/mode uppercased, band lowercased, all three
 * whitespace-stripped. ts is the QSO start in unix UTC seconds. */
typedef struct {
  gint64 id;                   /* 0 = not stored yet */
  gint64 ts;
  char  *call;
  char  *band;                 /* ADIF band, e.g. "40m" */
  double freq;                 /* MHz; <= 0 = unset */
  char  *mode;                 /* ADIF mode, e.g. "CW" */
  char  *submode;
  char  *rst_sent, *rst_rcvd;
  char  *gridsquare, *name, *qth;
  double tx_pwr;               /* W; <= 0 = unset */
  char  *comment;
  char  *qsl_rcvd, *qsl_sent;  /* ADIF QSL states (Y/N/R/Q/I) */
  char  *lotw_qsl_rcvd, *lotw_qsl_sent;
  char  *eqsl_qsl_rcvd, *eqsl_qsl_sent;
  char  *station_callsign, *my_gridsquare;
  char  *extras;               /* unmodeled ADIF fields, verbatim (M2) */
} LogflQso;

LogflQso *logfl_qso_new  (void);
LogflQso *logfl_qso_copy (const LogflQso *q);
void      logfl_qso_free (LogflQso *q);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (LogflQso, logfl_qso_free)

/* Opens (creating and/or migrating as needed) the store at path, which may be
 * ":memory:" for tests. NULL + error on failure — including a file whose
 * schema is newer than this app (never write into those). */
LogflStore *logfl_store_open  (const char *path, GError **error);
void        logfl_store_close (LogflStore *s);

/* CRUD. add normalizes q, stores it and sets q->id. update matches on q->id,
 * delete/get on id; a missing row is LOGFL_STORE_ERROR_NOT_FOUND. */
gboolean  logfl_store_add    (LogflStore *s, LogflQso *q, GError **error);
gboolean  logfl_store_update (LogflStore *s, LogflQso *q, GError **error);
gboolean  logfl_store_delete (LogflStore *s, gint64 id, GError **error);
LogflQso *logfl_store_get    (LogflStore *s, gint64 id, GError **error);

/* List newest-first. All filter fields optional: text is a case-insensitive
 * substring over call/name/qth/comment; band/mode match exactly (normalized).
 * limit 0 = unlimited. Returns a GPtrArray of LogflQso* (free-func set). */
typedef struct {
  const char *text;
  const char *band;
  const char *mode;
  guint       limit;
  guint       offset;
} LogflStoreQuery;

GPtrArray *logfl_store_list (LogflStore *s, const LogflStoreQuery *q,
                             GError **error);

/* Worked-before for a callsign: total QSO count, count on the given band,
 * count on band+mode, and the most recent ts (0 = never worked). band/mode
 * may be NULL (their counts come back 0). */
typedef struct {
  guint  n_total;
  guint  n_band;
  guint  n_band_mode;
  gint64 last_ts;
} LogflWorkedB4;

gboolean logfl_store_worked_b4 (LogflStore *s, const char *call,
                                const char *band, const char *mode,
                                LogflWorkedB4 *out, GError **error);

/* Dup check: does a QSO with the same call+band+mode exist within
 * ±window_s seconds of ts? */
gboolean logfl_store_dup_check (LogflStore *s, const char *call,
                                const char *band, const char *mode,
                                gint64 ts, guint window_s,
                                gboolean *is_dup, GError **error);

typedef struct {
  guint  n_qso;
  guint  n_calls;              /* distinct callsigns */
  gint64 first_ts, last_ts;    /* 0 when the log is empty */
} LogflStoreStats;

gboolean logfl_store_stats (LogflStore *s, LogflStoreStats *out,
                            GError **error);

/* Explicit transaction for bulk work (ADIF import). Nesting not supported. */
gboolean logfl_store_tx_begin    (LogflStore *s, GError **error);
gboolean logfl_store_tx_commit   (LogflStore *s, GError **error);
gboolean logfl_store_tx_rollback (LogflStore *s, GError **error);

G_END_DECLS

#endif /* LOGFL_LOG_STORE_H */
