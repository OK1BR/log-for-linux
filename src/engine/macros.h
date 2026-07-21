/* macros.h — contest-style F-key messaging (M5, docs/SCOPE.md).
 *
 * Pure GLib: token expansion, Run/S&P banks, ESM (Enter sends message).
 * No GTK; headless gate is log-macro-test. UI owns persistence and TX.
 *
 * Layout per bank: two rows of 12 — F1–F12 on row 1, free slots on row 2
 * with STOP as the last key (Esc). Empty free templates are unused, not stop.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#ifndef LOGFL_MACROS_H
#define LOGFL_MACROS_H

#include <glib.h>

G_BEGIN_DECLS

#define LOGFL_MACRO_N_ROW   12
#define LOGFL_MACRO_N_ROWS  2
#define LOGFL_MACRO_N_KEYS  (LOGFL_MACRO_N_ROW * LOGFL_MACRO_N_ROWS) /* 24 */
/* Last key on the second row — stop keyer (also Esc). */
#define LOGFL_MACRO_STOP_IDX (LOGFL_MACRO_N_KEYS - 1)

typedef enum {
  LOGFL_MACRO_BANK_RUN = 0,
  LOGFL_MACRO_BANK_SNP = 1,
  LOGFL_MACRO_BANK_N   = 2,
} LogflMacroBankId;

/* Positional ESM keys (N1MM-style: F1 CQ, F2 exchange, F3 TU). */
#define LOGFL_ESM_KEY_CQ   0
#define LOGFL_ESM_KEY_EXCH 1
#define LOGFL_ESM_KEY_TU   2

typedef struct {
  char *caption;               /* short label under F-number / free slot  */
  char *tmpl;                  /* CW text; empty = unused (except STOP)  */
} LogflMacroKey;

typedef struct {
  LogflMacroKey keys[LOGFL_MACRO_BANK_N][LOGFL_MACRO_N_KEYS];
} LogflMacroSet;

/* Heap-owned defaults (Run + S&P). Safe on a zeroed set. */
void logfl_macro_set_init_defaults (LogflMacroSet *set);
void logfl_macro_set_clear (LogflMacroSet *set);

const LogflMacroKey *logfl_macro_set_key (const LogflMacroSet *set,
                                          LogflMacroBankId bank, guint idx);

/* Replace caption/tmpl (copies). idx must be < LOGFL_MACRO_N_KEYS. */
void logfl_macro_set_set_key (LogflMacroSet *set, LogflMacroBankId bank,
                              guint idx, const char *caption,
                              const char *tmpl);

/* TRUE for the dedicated STOP slot (last of row 2). Empty free keys are not. */
gboolean logfl_macro_index_is_stop (guint idx);

/* Expand {MYCALL}, {CALL}, {RST}, and bare '!'. Caller frees. Empty tokens
 * collapse double spaces. NULL tmpl → empty string. */
char *logfl_macro_expand (const char *tmpl, const char *mycall,
                          const char *his_call, const char *rst);

/* --- ESM: Enter sends message ------------------------------------------- */

typedef enum {
  LOGFL_ESM_PHASE_READY = 0,   /* empty→CQ; call filled→EXCH then LOG */
  LOGFL_ESM_PHASE_LOG,         /* next Enter logs the QSO */
  LOGFL_ESM_PHASE_TU,          /* next Enter sends TU, then READY */
} LogflEsmPhase;

typedef enum {
  LOGFL_ESM_ACT_NONE = 0,
  LOGFL_ESM_ACT_SEND_MACRO,    /* out_key is F-index to send */
  LOGFL_ESM_ACT_LOG,           /* log the QSO from the entry row */
} LogflEsmAct;

/* Decide what Enter does. Does not mutate phase. out_key set when SEND. */
LogflEsmAct logfl_esm_decide (LogflEsmPhase phase, gboolean call_present,
                              guint *out_key);

/* Phase after a decided action was taken (or attempted). */
LogflEsmPhase logfl_esm_next (LogflEsmPhase phase, LogflEsmAct act,
                              gboolean call_present);

G_END_DECLS

#endif /* LOGFL_MACROS_H */
