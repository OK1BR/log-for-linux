/* macros.c — token expand, default banks, ESM state machine (M5).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "macros.h"

#include <string.h>

static void
set_key (LogflMacroKey *k, const char *cap, const char *tmpl)
{
  g_free (k->caption);
  g_free (k->tmpl);
  k->caption = g_strdup (cap ? cap : "");
  k->tmpl = g_strdup (tmpl ? tmpl : "");
}

void
logfl_macro_set_init_defaults (LogflMacroSet *set)
{
  g_return_if_fail (set != NULL);
  logfl_macro_set_clear (set);

  /* Row 1 F1–F7 useful defaults; F8–F12 free. Row 2 free except last = STOP.
   * Indices 0..11 = F1..F12, 12..23 = second row (23 = STOP). */
  static const struct { const char *c; const char *t; } run_filled[] = {
    { "CQ",   "CQ {MYCALL} {MYCALL} TEST" },
    { "EXCH", "{CALL} {RST}" },
    { "TU",   "TU {MYCALL}" },
    { "MY",   "{MYCALL}" },
    { "HIS",  "{CALL}" },
    { "AGN",  "AGN?" },
    { "QRZ",  "QRZ {MYCALL}" },
  };
  static const struct { const char *c; const char *t; } snp_filled[] = {
    { "ANS",  "{CALL} {MYCALL}" },
    { "EXCH", "{RST}" },
    { "TU",   "TU" },
    { "MY",   "{MYCALL}" },
    { "HIS",  "{CALL}" },
    { "AGN",  "AGN?" },
    { "NR",   "{CALL} {MYCALL} {MYCALL}" },
  };
  const guint n_filled = G_N_ELEMENTS (run_filled);

  for (guint i = 0; i < LOGFL_MACRO_N_KEYS; i++)
    {
      if (i < n_filled)
        {
          set_key (&set->keys[LOGFL_MACRO_BANK_RUN][i],
                   run_filled[i].c, run_filled[i].t);
          set_key (&set->keys[LOGFL_MACRO_BANK_SNP][i],
                   snp_filled[i].c, snp_filled[i].t);
        }
      else if (i == LOGFL_MACRO_STOP_IDX)
        {
          set_key (&set->keys[LOGFL_MACRO_BANK_RUN][i], "STOP", "");
          set_key (&set->keys[LOGFL_MACRO_BANK_SNP][i], "STOP", "");
        }
      else
        {
          /* Free slot — empty caption + template for the operator to fill. */
          set_key (&set->keys[LOGFL_MACRO_BANK_RUN][i], "", "");
          set_key (&set->keys[LOGFL_MACRO_BANK_SNP][i], "", "");
        }
    }
}

void
logfl_macro_set_clear (LogflMacroSet *set)
{
  if (!set)
    return;
  for (guint b = 0; b < LOGFL_MACRO_BANK_N; b++)
    for (guint i = 0; i < LOGFL_MACRO_N_KEYS; i++)
      {
        g_clear_pointer (&set->keys[b][i].caption, g_free);
        g_clear_pointer (&set->keys[b][i].tmpl, g_free);
      }
}

const LogflMacroKey *
logfl_macro_set_key (const LogflMacroSet *set, LogflMacroBankId bank,
                     guint idx)
{
  g_return_val_if_fail (set != NULL, NULL);
  g_return_val_if_fail (bank < LOGFL_MACRO_BANK_N, NULL);
  g_return_val_if_fail (idx < LOGFL_MACRO_N_KEYS, NULL);
  return &set->keys[bank][idx];
}

void
logfl_macro_set_set_key (LogflMacroSet *set, LogflMacroBankId bank,
                         guint idx, const char *caption, const char *tmpl)
{
  g_return_if_fail (set != NULL);
  g_return_if_fail (bank < LOGFL_MACRO_BANK_N);
  g_return_if_fail (idx < LOGFL_MACRO_N_KEYS);
  set_key (&set->keys[bank][idx], caption, tmpl);
}

gboolean
logfl_macro_index_is_stop (guint idx)
{
  return idx == LOGFL_MACRO_STOP_IDX;
}

char *
logfl_macro_expand (const char *tmpl, const char *mycall,
                    const char *his_call, const char *rst)
{
  if (!tmpl)
    return g_strdup ("");

  if (!mycall)
    mycall = "";
  if (!his_call)
    his_call = "";
  if (!rst || !*rst)
    rst = "599";

  GString *out = g_string_new (NULL);
  for (const char *p = tmpl; *p;)
    {
      if (g_str_has_prefix (p, "{MYCALL}"))
        {
          g_string_append (out, mycall);
          p += strlen ("{MYCALL}");
        }
      else if (g_str_has_prefix (p, "{CALL}"))
        {
          g_string_append (out, his_call);
          p += strlen ("{CALL}");
        }
      else if (g_str_has_prefix (p, "{RST}"))
        {
          g_string_append (out, rst);
          p += strlen ("{RST}");
        }
      else if (*p == '!')
        {
          g_string_append (out, his_call);
          p++;
        }
      else
        {
          g_string_append_c (out, *p);
          p++;
        }
    }

  char *s = g_string_free (out, FALSE);
  for (char *a = s; *a;)
    {
      if (a[0] == ' ' && a[1] == ' ')
        memmove (a, a + 1, strlen (a));
      else
        a++;
    }
  g_strstrip (s);
  return s;
}

LogflEsmAct
logfl_esm_decide (LogflEsmPhase phase, gboolean call_present, guint *out_key)
{
  if (out_key)
    *out_key = 0;

  /* After a successful log the entry call is usually cleared; still honor
   * the TU phase so the operator can finish the exchange on Enter. */
  if (!call_present)
    {
      if (phase == LOGFL_ESM_PHASE_TU)
        {
          if (out_key)
            *out_key = LOGFL_ESM_KEY_TU;
          return LOGFL_ESM_ACT_SEND_MACRO;
        }
      if (out_key)
        *out_key = LOGFL_ESM_KEY_CQ;
      return LOGFL_ESM_ACT_SEND_MACRO;
    }

  switch (phase)
    {
    case LOGFL_ESM_PHASE_READY:
      if (out_key)
        *out_key = LOGFL_ESM_KEY_EXCH;
      return LOGFL_ESM_ACT_SEND_MACRO;
    case LOGFL_ESM_PHASE_LOG:
      return LOGFL_ESM_ACT_LOG;
    case LOGFL_ESM_PHASE_TU:
      if (out_key)
        *out_key = LOGFL_ESM_KEY_TU;
      return LOGFL_ESM_ACT_SEND_MACRO;
    default:
      return LOGFL_ESM_ACT_NONE;
    }
}

LogflEsmPhase
logfl_esm_next (LogflEsmPhase phase, LogflEsmAct act, gboolean call_present)
{
  (void) call_present;

  switch (act)
    {
    case LOGFL_ESM_ACT_SEND_MACRO:
      if (phase == LOGFL_ESM_PHASE_READY && call_present)
        return LOGFL_ESM_PHASE_LOG;
      if (phase == LOGFL_ESM_PHASE_TU)
        return LOGFL_ESM_PHASE_READY;
      /* CQ with empty call stays READY. */
      return LOGFL_ESM_PHASE_READY;
    case LOGFL_ESM_ACT_LOG:
      return LOGFL_ESM_PHASE_TU;
    case LOGFL_ESM_ACT_NONE:
    default:
      return phase;
    }
}
