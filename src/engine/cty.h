/* cty.h — country/continent from a callsign prefix (AD1C cty.dat).
 *
 * Loads the standard Amateur Radio Country File (country-files.com,
 * MIT-licensed, snapshot bundled in data/) and answers "which DXCC entity
 * and continent is this call?". That is what contest validity rules need
 * (WAE: EU works non-EU only; EUHFC: EU only; CQ WW: own country scores
 * zero) and what the entry row shows for a new call ("New call from
 * Czech Republic").
 *
 * Matching follows the cty.dat conventions: exact "=CALL" entries win over
 * prefixes, then the longest matching prefix. Portable calls take the
 * designator part ("DL/OK1BR" → DL, "OK1BR/P" → OK1BR); /MM and /AM have
 * no country by definition and miss. Alias zone/continent overrides
 * "(cq)[itu]{cont}" are applied; "<lat/long>" and "~tz~" are skipped.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_CTY_H
#define LOGFL_CTY_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _LogflCty LogflCty;

/* One lookup answer. Strings are owned by the LogflCty — valid until free. */
typedef struct {
  const char *country;         /* entity name, e.g. "Czech Republic" */
  const char *prefix;          /* primary prefix, e.g. "OK" */
  char        continent[3];    /* "EU", "NA", … */
  int         cq_zone;
  int         itu_zone;
  gboolean    waedc_only;      /* '*' entity: on the WAE list, not DXCC */
} LogflCtyInfo;

/* Parse a cty.dat file. NULL + error when unreadable or no entity parses. */
LogflCty *logfl_cty_load (const char *path, GError **error);
void      logfl_cty_free (LogflCty *c);

/* Find the bundled file: $XDG_DATA_HOME/log-for-linux/cty.dat first (a
 * user-refreshed snapshot wins), then each system data dir, then
 * ./data/cty.dat for a build-dir run. NULL when none exists. */
char *logfl_cty_locate (void);

/* Entity for a callsign; FALSE when nothing matches (also /MM, /AM).
 * call may be any case and carry portable designators. */
gboolean logfl_cty_lookup (LogflCty *c, const char *call, LogflCtyInfo *out);

/* Snapshot version from the "=VERyyyymmdd" marker entry, e.g. "20260803";
 * NULL when the file carries none. Owned by the LogflCty. */
const char *logfl_cty_version (LogflCty *c);

guint logfl_cty_n_entities (LogflCty *c);

G_END_DECLS

#endif /* LOGFL_CTY_H */
