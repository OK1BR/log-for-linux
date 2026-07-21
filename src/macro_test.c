/*
 * log-macro-test — offline gate for M5 macros (expand, banks, ESM).
 * No radio, no GUI.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include <glib.h>
#include <string.h>

#include "macros.h"

static void
test_expand_tokens (void)
{
  char *s = logfl_macro_expand ("CQ {MYCALL} {MYCALL} TEST", "OK1BR",
                                "W1AW", "599");
  g_assert_cmpstr (s, ==, "CQ OK1BR OK1BR TEST");
  g_free (s);

  s = logfl_macro_expand ("{CALL} {RST}", "OK1BR", "OK2ABC", "579");
  g_assert_cmpstr (s, ==, "OK2ABC 579");
  g_free (s);

  s = logfl_macro_expand ("! de {MYCALL}", "OK1BR", "DL1AA", NULL);
  g_assert_cmpstr (s, ==, "DL1AA de OK1BR");
  g_free (s);

  /* Empty his call collapses double spaces; strip edges. */
  s = logfl_macro_expand ("{CALL} {RST} {MYCALL}", "OK1BR", "", "599");
  g_assert_cmpstr (s, ==, "599 OK1BR");
  g_free (s);

  s = logfl_macro_expand (NULL, "OK1BR", "X", "1");
  g_assert_cmpstr (s, ==, "");
  g_free (s);

  /* Blank RST falls back to 599. */
  s = logfl_macro_expand ("{RST}", "A", "B", "");
  g_assert_cmpstr (s, ==, "599");
  g_free (s);
}

static void
test_defaults_and_banks (void)
{
  LogflMacroSet set;
  memset (&set, 0, sizeof set);
  logfl_macro_set_init_defaults (&set);

  const LogflMacroKey *k =
      logfl_macro_set_key (&set, LOGFL_MACRO_BANK_RUN, 0);
  g_assert_nonnull (k);
  g_assert_cmpstr (k->caption, ==, "CQ");
  g_assert_true (strstr (k->tmpl, "{MYCALL}") != NULL);

  /* F8 is free, not stop; STOP is last of row 2 (index 15). */
  g_assert_cmpuint (LOGFL_MACRO_N_KEYS, ==, 16);
  g_assert_false (logfl_macro_index_is_stop (7));
  k = logfl_macro_set_key (&set, LOGFL_MACRO_BANK_RUN, 7);
  g_assert_cmpstr (k->caption, ==, "");
  g_assert_cmpstr (k->tmpl, ==, "");

  g_assert_true (logfl_macro_index_is_stop (LOGFL_MACRO_STOP_IDX));
  g_assert_cmpuint (LOGFL_MACRO_STOP_IDX, ==, 15);
  k = logfl_macro_set_key (&set, LOGFL_MACRO_BANK_RUN, LOGFL_MACRO_STOP_IDX);
  g_assert_cmpstr (k->caption, ==, "STOP");
  g_assert_cmpstr (k->tmpl, ==, "");

  /* Second-row free slot empty. */
  k = logfl_macro_set_key (&set, LOGFL_MACRO_BANK_RUN, 8);
  g_assert_cmpstr (k->tmpl, ==, "");

  k = logfl_macro_set_key (&set, LOGFL_MACRO_BANK_SNP, 0);
  g_assert_cmpstr (k->caption, ==, "ANS");

  logfl_macro_set_set_key (&set, LOGFL_MACRO_BANK_RUN, 0, "TEST",
                           "CQ {MYCALL}");
  k = logfl_macro_set_key (&set, LOGFL_MACRO_BANK_RUN, 0);
  g_assert_cmpstr (k->caption, ==, "TEST");
  g_assert_cmpstr (k->tmpl, ==, "CQ {MYCALL}");

  logfl_macro_set_clear (&set);
  k = logfl_macro_set_key (&set, LOGFL_MACRO_BANK_RUN, 0);
  g_assert_null (k->caption);
  g_assert_null (k->tmpl);
}

static void
test_esm_cycle (void)
{
  guint key = 99;
  LogflEsmPhase ph = LOGFL_ESM_PHASE_READY;

  /* Empty call always CQ. */
  g_assert_cmpint (logfl_esm_decide (ph, FALSE, &key), ==,
                   LOGFL_ESM_ACT_SEND_MACRO);
  g_assert_cmpuint (key, ==, LOGFL_ESM_KEY_CQ);
  ph = logfl_esm_next (ph, LOGFL_ESM_ACT_SEND_MACRO, FALSE);
  g_assert_cmpint (ph, ==, LOGFL_ESM_PHASE_READY);

  /* Call filled: EXCH → LOG → TU → READY. */
  g_assert_cmpint (logfl_esm_decide (ph, TRUE, &key), ==,
                   LOGFL_ESM_ACT_SEND_MACRO);
  g_assert_cmpuint (key, ==, LOGFL_ESM_KEY_EXCH);
  ph = logfl_esm_next (ph, LOGFL_ESM_ACT_SEND_MACRO, TRUE);
  g_assert_cmpint (ph, ==, LOGFL_ESM_PHASE_LOG);

  g_assert_cmpint (logfl_esm_decide (ph, TRUE, &key), ==, LOGFL_ESM_ACT_LOG);
  ph = logfl_esm_next (ph, LOGFL_ESM_ACT_LOG, TRUE);
  g_assert_cmpint (ph, ==, LOGFL_ESM_PHASE_TU);

  g_assert_cmpint (logfl_esm_decide (ph, TRUE, &key), ==,
                   LOGFL_ESM_ACT_SEND_MACRO);
  g_assert_cmpuint (key, ==, LOGFL_ESM_KEY_TU);
  ph = logfl_esm_next (ph, LOGFL_ESM_ACT_SEND_MACRO, TRUE);
  g_assert_cmpint (ph, ==, LOGFL_ESM_PHASE_READY);

  /* After log the call is often cleared — still send TU, then READY. */
  ph = LOGFL_ESM_PHASE_TU;
  g_assert_cmpint (logfl_esm_decide (ph, FALSE, &key), ==,
                   LOGFL_ESM_ACT_SEND_MACRO);
  g_assert_cmpuint (key, ==, LOGFL_ESM_KEY_TU);
  ph = logfl_esm_next (ph, LOGFL_ESM_ACT_SEND_MACRO, FALSE);
  g_assert_cmpint (ph, ==, LOGFL_ESM_PHASE_READY);

  /* Empty call + READY → CQ. */
  g_assert_cmpint (logfl_esm_decide (ph, FALSE, &key), ==,
                   LOGFL_ESM_ACT_SEND_MACRO);
  g_assert_cmpuint (key, ==, LOGFL_ESM_KEY_CQ);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/macros/expand", test_expand_tokens);
  g_test_add_func ("/macros/banks", test_defaults_and_banks);
  g_test_add_func ("/macros/esm", test_esm_cycle);
  return g_test_run ();
}
