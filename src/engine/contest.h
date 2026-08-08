/* contest.h — contest exchange templates (docs/SCOPE.md, contest milestone).
 *
 * A contest's template (LogflExchDef) lists the fields the operator copies
 * from the other station and how each value maps onto ADIF: serials go to
 * SRX/STX, everything else to a named ADIF field (CQZ, ITUZ, CONT, …) or to
 * SRX_STRING. Values routed to fields the store does not model are appended
 * to the QSO's extras as regular ADIF tags, so export stays lossless for
 * free. Definitions serialize to GKeyFile text held in contest.exch_def
 * (log_store.h); a contest carries its own copy, so editing a preset never
 * rewrites history.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_CONTEST_H
#define LOGFL_CONTEST_H

#include "cty.h"
#include "log_store.h"

G_BEGIN_DECLS

#define LOGFL_CONTEST_ERROR logfl_contest_error_quark ()
GQuark logfl_contest_error_quark (void);

typedef enum {
  LOGFL_CONTEST_ERROR_PARSE,   /* malformed exchange definition */
} LogflContestError;

/* How a received value routes into ADIF:
 *   SERIAL — the serial slot; digits → adif_num (SRX), else adif_text.
 *   NUMBER — numeric expected; digits → adif_num, else adif_text fallback.
 *   TEXT   — always adif_text (district codes may be digits, e.g. "01").
 *   AUTO   — digits → adif_num, else adif_text (mixed exchanges: OK/OM DX
 *            where DX stations send a serial and OK/OM a district). */
typedef enum {
  LOGFL_EXCH_SERIAL,
  LOGFL_EXCH_NUMBER,
  LOGFL_EXCH_TEXT,
  LOGFL_EXCH_AUTO,
} LogflExchFieldType;

typedef struct {
  char *key;                   /* stable id (keyfile group suffix) */
  char *label;                 /* entry-row caption, e.g. "Zone" */
  LogflExchFieldType type;
  char *adif_num;              /* ADIF field for digit values (SRX, CQZ, …) */
  char *adif_text;             /* ADIF field for text values (SRX_STRING, …) */
  gboolean required;           /* UI hint: don't log while empty */
} LogflExchField;

/* Who counts in this contest — each rule verified against the sponsor's
 * official rules (see the preset comments for sources; house rule since
 * 2026-08-08: never add a preset without checking this).
 *   ALL     — every QSO is valid.
 *   EU_DX   — only QSOs crossing the EU boundary (WAE: a European station
 *             works non-Europeans only, and vice versa).
 *   EU_ONLY — only QSOs with European stations (EUHFC). */
typedef enum {
  LOGFL_COUNTS_ALL = 0,
  LOGFL_COUNTS_EU_DX,
  LOGFL_COUNTS_EU_ONLY,
} LogflExchCounts;

typedef struct {
  gboolean tx_serial;          /* sent exchange includes an auto serial */
  GPtrArray *fields;           /* received exchange, LogflExchField* */
  LogflExchCounts counts;      /* which QSOs are valid at all */
  gboolean zero_own_country;   /* valid but 0 points (CQ WW own country) */
} LogflExchDef;

/* Serialized form is GKeyFile text:
 *   [exchange]
 *   tx_serial=true
 *   fields=nr;
 *   counts=eu-dx           # all|eu-dx|eu-only, missing = all
 *   zero_own_country=false
 *   [field:nr]
 *   label=Nr
 *   type=serial            # serial|number|text|auto
 *   adif_num=SRX           # defaults: SRX / SRX_STRING when omitted
 *   adif_text=SRX_STRING
 *   required=true
 */
LogflExchDef *logfl_exch_def_parse     (const char *text, GError **error);
char         *logfl_exch_def_serialize (const LogflExchDef *def);
void          logfl_exch_def_free      (LogflExchDef *def);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (LogflExchDef, logfl_exch_def_free)

/* Applies an operator-entered exchange onto a QSO about to be stored.
 * values runs parallel to def->fields (NULL/empty entries are skipped);
 * received values fill srx/srx_string or append ADIF tags to extras.
 * Sent side: stx = serial when def->tx_serial, stx_string = my_exch.
 * Routed values are uppercased (contest exchange convention). */
void logfl_exch_apply (const LogflExchDef *def,
                       const char *const *values, guint n_values,
                       const char *my_exch, guint serial,
                       LogflQso *qso);

/* "1" → "001" — the on-air serial convention (grows past 999 naturally). */
char *logfl_exch_serial_format (guint serial);

/* Does a QSO with them count under this contest's rule? mine/theirs come
 * from the cty resolver; pass NULL for either when unresolved — unknown
 * stations get the benefit of the doubt (VALID), never a false alarm. */
typedef enum {
  LOGFL_QSO_VALID = 0,
  LOGFL_QSO_ZERO_POINTS,       /* counts, but 0 QSO points (still a mult) */
  LOGFL_QSO_NOT_VALID,         /* no contest QSO under the rules */
} LogflQsoValidity;

LogflQsoValidity logfl_contest_qso_validity (const LogflExchDef *def,
                                             const LogflCtyInfo *mine,
                                             const LogflCtyInfo *theirs);

/* Built-in presets: name + ADIF CONTEST_ID prefill + serialized definition.
 * A new contest copies (and may edit) the definition — the preset list is a
 * starting point, not a registry. */
typedef struct {
  const char *name;            /* "CQ WPX" */
  const char *adif_id;         /* CONTEST_ID prefill; NULL = none */
  const char *exch_def;        /* serialized LogflExchDef */
  const char *my_exch_hint;    /* UI placeholder for the sent exchange */
} LogflContestPreset;

const LogflContestPreset *logfl_contest_presets (guint *n);

G_END_DECLS

#endif /* LOGFL_CONTEST_H */
