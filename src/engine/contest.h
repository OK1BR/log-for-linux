/* contest.h — contest exchange templates (M9).
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
  char *cab_placeholder;       /* what the Cabrillo QSO line carries in this
                                  field's slot when a side did not send it
                                  (CQ WW RTTY: "DX" for everyone outside
                                  W/VE); NULL = the slot just stays empty */
} LogflExchField;

/* Who counts in this contest — each rule verified against the sponsor's
 * official rules (see the preset comments for sources; house rule since
 * 2026-08-08: never add a preset without checking this).
 *   ALL     — every QSO is valid.
 *   EU_DX   — only QSOs crossing the EU boundary (WAE: a European station
 *             works non-Europeans only, and vice versa).
 *   EU_ONLY — only QSOs with European stations (EUHFC).
 *   ENTITIES — only QSOs with stations of the listed DXCC entities (SAC:
 *             the Scandinavian ones). Judged on their side alone, like
 *             EU_ONLY — the seat inside the list is not modeled. */
typedef enum {
  LOGFL_COUNTS_ALL = 0,
  LOGFL_COUNTS_EU_DX,
  LOGFL_COUNTS_EU_ONLY,
  LOGFL_COUNTS_ENTITIES,
} LogflExchCounts;

/* --- scoring rules (LOG-3, 2026-08-28) -----------------------------------
 * Every encoded preset value was verified against the sponsor's official
 * rules on 2026-08-28 (sources at each preset, house rule: never encode
 * unverified). The claimed score is an ESTIMATE from my seat — the
 * sponsor's robot rescoring is the authority. */

/* One points-rule term. An ordered list, first match wins, judged from my
 * seat via the cty resolver. */
typedef enum {
  LOGFL_PTS_COUNTRY,           /* their DXCC primary prefix equals arg */
  LOGFL_PTS_OWN_COUNTRY,       /* same DXCC entity as mine */
  LOGFL_PTS_SAME_CONT,
  LOGFL_PTS_OTHER_CONT,
  LOGFL_PTS_SAME_ZONE,         /* same ITU zone; theirs from the received
                                  exchange when numeric, cty fallback */
  LOGFL_PTS_EXCH_TEXT,         /* received exchange is text, not a number
                                  (IARU HQ/official stations) */
  LOGFL_PTS_DEFAULT,           /* always matches */
} LogflPtsKind;

typedef struct {
  LogflPtsKind kind;
  char *arg;                   /* COUNTRY: the prefix ("YO"); else NULL */
  int points;                  /* 20 m and shorter */
  int points_low;              /* 160/80/40 m (== points unless "N/L") */
} LogflPtsTerm;

/* Multiplier sources — OR-able, each new value = one multiplier. */
typedef enum {
  LOGFL_MULT_COUNTRY       = 1 << 0, /* DXCC/WAE entity (cty prefix) */
  LOGFL_MULT_COUNTRY_AREAS = 1 << 1, /* … with numerical call areas split in
                                        W VE VK ZL ZS JA BY PY RA8/9/0
                                        (WAE DX rules §6) */
  LOGFL_MULT_CQZONE        = 1 << 2, /* CQ zone (exchange, cty fallback) */
  LOGFL_MULT_ZONE          = 1 << 3, /* ITU zone (exchange, cty fallback) */
  LOGFL_MULT_EXCH          = 1 << 4, /* received exchange text, digits too
                                        (EUHFC years, YO counties) */
  LOGFL_MULT_EXCH_TEXT     = 1 << 5, /* … its non-numeric tokens only (IARU
                                        HQ; the W/VE QTH behind the zone in
                                        CQ WW RTTY) */
  LOGFL_MULT_PREFIX        = 1 << 6, /* WPX prefix of the call */
  LOGFL_MULT_CALL_AREAS    = 1 << 7, /* prefix number 0-9 within each listed
                                        entity, the entity itself elsewhere
                                        (SAC rules §8.2) */
} LogflMultSource;

/* --- Cabrillo entry categories -------------------------------------------
 * Which values a CATEGORY-* header tag may take in this contest, from the
 * sponsor's rules and the sponsor's own Cabrillo page (sources and read
 * dates at each preset). 2026-09-20: a SAC log from OK went out as
 * CATEGORY-BAND: 20M — single band is for Scandinavians only there, and
 * the export dialog offered every contest the same Cabrillo v3 lists.
 * Per-tag lists only: what the rules say about combinations (QRP all-band
 * only, …) is not modeled — the sponsor's robot is the authority. */
typedef enum {
  LOGFL_CAB_OPERATOR,
  LOGFL_CAB_BAND,
  LOGFL_CAB_POWER,
  LOGFL_CAB_MODE,
  LOGFL_CAB_TRANSMITTER,
  LOGFL_CAB_ASSISTED,
  LOGFL_CAB_OVERLAY,
  LOGFL_CAB_STATION,
  LOGFL_CAB_N_TAGS
} LogflCabTag;

typedef struct {
  gboolean tx_serial;          /* sent exchange includes an auto serial */
  GPtrArray *fields;           /* received exchange, LogflExchField* */
  LogflExchCounts counts;      /* which QSOs are valid at all */
  char **counts_entities;      /* ENTITIES: cty primary prefixes, NULL-
                                  terminated; NULL under any other rule */
  char *counts_name;           /* ENTITIES: what the list is called, for the
                                  operator ("Scandinavian"); may be NULL */
  gboolean zero_own_country;   /* valid but 0 points (CQ WW own country) */
  GArray *points;              /* LogflPtsTerm, ordered; NULL = no rule */
  guint mult;                  /* LogflMultSource mask; 0 = no rule */
  char *mult_exch_from;        /* EXCH counts only from this DXCC prefix
                                  (CVA: PY states; NULL = from anyone) */
  char **mult_exch_text_from;  /* EXCH_TEXT counts only from these DXCC
                                  prefixes, NULL-terminated (CQ WW RTTY:
                                  K, VE); NULL = from anyone */
  char **mult_area_entities;   /* CALL_AREAS: the entities split by prefix
                                  number, NULL-terminated */
  gboolean mult_per_contest;   /* mults once per contest (WPX prefixes),
                                  not once per band */
  GHashTable *mult_weight;     /* band ("80m") → weight int; NULL = ×1
                                  everywhere (WAE band bonus) */
  char **cab[LOGFL_CAB_N_TAGS];        /* allowed header values per tag,
                                  NULL-terminated, the default first; NULL =
                                  no rule; an empty list = the sponsor's
                                  header does not know the tag */
  char **cab_inside[LOGFL_CAB_N_TAGS]; /* … for an entrant inside
                                  counts_entities (SAC: Scandinavia); NULL =
                                  the same as cab[] */
} LogflExchDef;

/* Serialized form is GKeyFile text:
 *   [exchange]
 *   tx_serial=true
 *   fields=nr;
 *   counts=eu-dx           # all|eu-dx|eu-only|entities:LA,SM,…; missing = all
 *   counts_name=Scandinavian   # entities: only — names the list in the UI
 *   zero_own_country=false
 *   points=country:YO=8;own-country=1;same-cont=2;other-cont=4;
 *                          # ordered, first match wins; N/L = low-band
 *                          # (160/80/40) override, e.g. other-cont=3/6
 *   mult=exch:YO+country   # country|country-areas|cqzone|zone|exch[:PFX]|
 *                          # exch-text[:K,VE,…]|prefix|call-areas:LA,SM,…,
 *                          # joined with +
 *   mult_scope=contest     # band (default) | contest
 *   mult_weight=80m:4;40m:3;20m:2;15m:2;10m:2   # WAE band bonus
 *   [cabrillo]             # optional; allowed CATEGORY-* values per tag:
 *   band=ALL;LOW-BAND;     # operator|band|power|mode|transmitter|assisted|
 *                          # overlay|station — the default first; an empty
 *                          # value = the tag stays out of the header; a
 *                          # missing key = no rule (the Cabrillo v3 list)
 *   band_inside=ALL;80M;40M;   # KEY_inside: the list for an entrant inside
 *                          # the counts=entities: list (needs that rule)
 *   [field:nr]
 *   label=Nr
 *   type=serial            # serial|number|text|auto
 *   adif_num=SRX           # defaults: SRX / SRX_STRING when omitted
 *   adif_text=SRX_STRING
 *   required=true
 *   cab_placeholder=DX     # the Cabrillo QSO line's stand-in when a side
 *                          # did not send this field; one token
 * An unknown points term or mult source fails the parse loudly, like an
 * unknown counts= rule — a build must never silently score wrong. NB the
 * round-trip asymmetry: this build carries points/mult through
 * parse→serialize, but an older build editing the contest drops them on
 * reserialize (same as counts= before it existed) — the failure mode is
 * the score display disappearing, never wrong data. [cabrillo] rides the
 * same way: dropped by an older build, the export dialog is back to the
 * plain Cabrillo v3 lists. */
LogflExchDef *logfl_exch_def_parse     (const char *text, GError **error);
char         *logfl_exch_def_serialize (const LogflExchDef *def);
void          logfl_exch_def_free      (LogflExchDef *def);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (LogflExchDef, logfl_exch_def_free)

/* The values a CATEGORY-* tag may take for an entrant seated at mine (the
 * cty answer for my own call). NULL = the def has no rule for the tag and
 * the caller keeps its Cabrillo v3 list; an empty list = leave the tag out
 * of the header. mine NULL or unresolved reads as outside the entity list
 * — the presets are written from that seat. */
const char *const *logfl_exch_def_cab_values (const LogflExchDef *def,
                                              LogflCabTag tag,
                                              const LogflCtyInfo *mine);

/* Applies an operator-entered exchange onto a QSO about to be stored.
 * values runs parallel to def->fields (NULL/empty entries are skipped);
 * received values fill srx/srx_string or append ADIF tags to extras.
 * Sent side: stx = serial when def->tx_serial, stx_string = my_exch.
 * Routed values are uppercased (contest exchange convention). Several
 * fields join into srx_string on a space in field order, empties skipped
 * — the string reads as one token per field sent (CQ WW RTTY: "05 MA"
 * from a W, "14" from a DL), which is how the scorer and the Cabrillo
 * writer take it apart again. */
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

/* One-time repair for contests created before validity rules existed: a
 * stored exch_def carrying neither counts nor zero_own_country gets the
 * rule its ADIF id implies (DARC-WAEDC* → eu-dx, EU-HF → eu-only,
 * CQ-WW* → zero_own_country) written back to the store. Since LOG-3 the
 * same pass also backfills points=/mult= for contests whose ADIF id maps
 * to a preset scoring rule and whose def names neither key, and the
 * [cabrillo] category lists for a def without that group — matched on the
 * whole ADIF id there, as CQ-WW-RTTY must not inherit the CW/SSB lists.
 * Defs that name a key — including an explicit counts=all — are left
 * alone, so an operator's own edit is never overridden. Returns how many
 * contests were updated; on a store error returns what was done and sets
 * error. */
guint logfl_contest_backfill_validity (LogflStore *s, GError **error);

/* --- scoring ------------------------------------------------------------ */

/* Per-QSO score annotation. */
typedef struct {
  int points;
  char *mult;                  /* space-joined labels of the multipliers
                                  this QSO brought first; NULL = none */
} LogflQsoScore;

typedef struct {
  gboolean have_points;        /* def carries points= */
  gboolean have_mult;          /* def carries mult= */
  gint64 points;               /* QSO points over the whole contest */
  gint64 mults;                /* multipliers, band-weighted (WAE) */
  gint64 total;                /* points × mults; just points w/o mult= */
} LogflContestTotals;

/* Scores one contest's QSOs from my seat. qsos is the logfl_store_list
 * result (ts DESC, id DESC) — walked oldest-first internally. Dupes (same
 * call+band+mode as an earlier QSO) score 0 and bring no multiplier;
 * validity applies (NOT_VALID = nothing, ZERO_POINTS = 0 points but the
 * multiplier still counts — CQ WW own country). Returns a qso-id →
 * LogflQsoScore* table (free with g_hash_table_unref) and fills totals;
 * NULL (totals zeroed) when the def carries no rule, or when my own call
 * is missing or unresolved — an estimate from an unknown seat would be a
 * guess, not a score. */
GHashTable *logfl_contest_score (const LogflExchDef *def,
                                 LogflCty *cty, const char *my_call,
                                 const GPtrArray *qsos,
                                 LogflContestTotals *totals);

/* WPX prefix of a call (cqwpx.com/rules §V.C, the common cases): up to and
 * including the last digit ("OK1BR"→"OK1", "LY1000XX"→"LY1000"); no digit →
 * first two characters + "0" ("XEFTJW"→"XE0"); a portable designator
 * becomes the prefix — lettered one directly ("PA/N8BJQ"→"PA0"), digit one
 * swaps the call area ("K5DJ/1"→"K1"). /P /M /MM /AM /QRP are ignored. */
char *logfl_wpx_prefix (const char *call);

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
