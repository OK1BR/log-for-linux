/* win.c — main logbook window: entry row + worked-B4, footer with UTC and
 * active TCI status; separate QSO-list window (table, search, edit, delete,
 * QSY). ADIF import/export and preferences live on the main window menu
 * (M3/M4). Edit loads a row into the entry strip and uses logfl_store_update.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "win.h"

#include <string.h>

#include "adif.h"
#include "engine.h"
#include "log_store.h"
#include "macros.h"
#include "qso_row.h"
#include "settings.h"
#include "tci_client.h"

#define TCI_RETRY_S 5

/* Live entry: same call+band+mode within this window asks before logging. */
#define DUP_WINDOW_S 300
/* ADIF import: only an identical timestamp is a dup (matches log-adif-test
 * and avoids collapsing intentional re-QSOs hours apart in a bulk file). */
#define ADIF_IMPORT_DUP_WINDOW_S 0
/* Debounce search so each keystroke does not re-query the whole store. */
#define SEARCH_DEBOUNCE_MS 250

/* Must cover every name returned by logfl_adif_band_for_freq(). */
static const char *bands[] = {
  "2190m", "630m", "160m", "80m", "60m", "40m", "30m", "20m", "17m",
  "15m", "12m", "10m", "6m", "4m", "2m", "1.25m", "70cm", "33cm", "23cm",
  NULL
};
static const char *modes[] = { "CW", "SSB", "FT8", "FT4", "RTTY", "PSK31",
                               "FM", "AM", NULL };

enum { COL_UTC, COL_CALL, COL_BAND, COL_FREQ, COL_MODE, COL_RST,
       COL_NAME, COL_COMMENT };

struct _LogflWindow {
  AdwApplicationWindow parent_instance;

  LogflStore *store;
  char *db_path;
  char *store_open_error;      /* non-NULL when open failed; shown once */
  LogflSettings settings;      /* ~/.config/log-for-linux/settings.ini   */

  GListStore *rows;
  GtkSingleSelection *selection;

  AdwWindowTitle *title;
  AdwToastOverlay *toasts;
  GtkWidget *log_win;          /* separate QSO table window (hide-on-close) */
  AdwWindowTitle *log_title;
  GtkWidget *search;
  GtkWidget *call, *rst_s, *rst_r, *freq, *name, *comment;
  GtkWidget *band_dd, *mode_dd;
  GtkWidget *wb4_label, *clock_label, *tci_label;
  GtkWidget *delete_btn;
  GtkWidget *edit_btn;         /* log window: load selected into entry */
  GtkWidget *log_btn;          /* "Log QSO" / "Save QSO" on main window */
  GtkWidget *cancel_edit_btn;  /* visible only while editing */
  GtkWidget *macro_btns[LOGFL_MACRO_N_KEYS];
  GtkWidget *bank_run_btn;
  GtkWidget *bank_snp_btn;
  GtkWidget *esm_hint;         /* short ESM status under macro bar */

  guint clock_id;
  guint search_id;             /* debounce timeout for search-changed */
  guint tci_retry_id;          /* reconnect timer when TCI is down */
  gboolean syncing_freq;       /* guard against freq↔band feedback */
  gboolean syncing_tci;        /* guard: applying radio state to entry */
  gboolean tci_connecting;     /* connect thread in flight */
  guint tci_epoch;             /* bumps on reconnect; stale jobs drop */
  LogflTciClient *tci;
  LogflQso *pending;           /* QSO awaiting dup confirmation */
  LogflQso *editing;           /* non-NULL: entry row is editing this QSO */
  gint64 pending_delete_id;
  gboolean delete_confirm_open; /* async delete dialog is up */
  LogflEsmPhase esm_phase;     /* M5 Enter-sends-message state */
  gboolean esm_force_log;      /* ESM LOG step → bypass ESM on Enter */
};

G_DEFINE_FINAL_TYPE (LogflWindow, logfl_window, ADW_TYPE_APPLICATION_WINDOW)

static void refresh_esm_hint (LogflWindow *self);
static GtkWidget *labeled (const char *caption, GtkWidget *child);

/* --- small helpers ------------------------------------------------------ */

static void
toast (LogflWindow *self, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  char *msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  adw_toast_overlay_add_toast (self->toasts, adw_toast_new (msg));
  g_free (msg);
}

static char *
fmt_freq (double mhz)
{
  if (mhz <= 0)
    return g_strdup ("");
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (buf, sizeof buf, "%.4f", mhz);
  char *last = buf + strlen (buf) - 1;
  while (last > buf && *last == '0')
    *last-- = '\0';
  if (*last == '.')
    *last = '\0';
  return g_strdup (buf);
}

static const char *
dd_selected (GtkWidget *dd, const char **table)
{
  guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (dd));
  return i == GTK_INVALID_LIST_POSITION ? NULL : table[i];
}

static const char *
entry_text (GtkWidget *e)
{
  return gtk_editable_get_text (GTK_EDITABLE (e));
}

/* --- data reload -------------------------------------------------------- */

static void
reload (LogflWindow *self)
{
  if (!self->store)
    {
      if (self->rows)
        g_list_store_remove_all (self->rows);
      adw_window_title_set_subtitle (self->title, "log store unavailable");
      if (self->log_title)
        adw_window_title_set_subtitle (self->log_title, "log store unavailable");
      return;
    }

  GError *err = NULL;
  const char *text = (self->search && entry_text (self->search)[0])
                         ? entry_text (self->search)
                         : NULL;
  LogflStoreQuery q = { .text = text };
  GPtrArray *list = logfl_store_list (self->store, &q, &err);

  if (self->rows)
    g_list_store_remove_all (self->rows);
  if (!list)
    {
      toast (self, "Query failed: %s", err->message);
      g_clear_error (&err);
      return;
    }
  for (guint i = 0; i < list->len; i++)
    {
      LogflQsoRow *row = logfl_qso_row_new (logfl_qso_copy (list->pdata[i]));
      g_list_store_append (self->rows, row);
      g_object_unref (row);
    }
  g_ptr_array_unref (list);

  LogflStoreStats st;
  if (logfl_store_stats (self->store, &st, NULL))
    {
      char *sub = g_strdup_printf ("%u QSO · %u calls", st.n_qso, st.n_calls);
      adw_window_title_set_subtitle (self->title, sub);
      if (self->log_title)
        adw_window_title_set_subtitle (self->log_title, sub);
      g_free (sub);
    }
}

static gboolean
search_debounce_fire (gpointer user_data)
{
  LogflWindow *self = user_data;
  self->search_id = 0;
  reload (self);
  return G_SOURCE_REMOVE;
}

static void
on_search_changed (LogflWindow *self)
{
  g_clear_handle_id (&self->search_id, g_source_remove);
  self->search_id = g_timeout_add (SEARCH_DEBOUNCE_MS, search_debounce_fire,
                                   self);
}

/* --- TCI (M4) ----------------------------------------------------------- */

static void tci_schedule_connect (LogflWindow *self);
static gboolean tci_connect_kick (gpointer user_data);
static void update_wb4 (LogflWindow *self);

static void
tci_set_status (LogflWindow *self, const char *txt)
{
  if (self->tci_label)
    gtk_label_set_text (GTK_LABEL (self->tci_label), txt ? txt : "");
}

static void
select_mode_string (LogflWindow *self, const char *mode)
{
  if (!mode)
    return;
  for (guint i = 0; modes[i]; i++)
    if (g_str_equal (modes[i], mode))
      {
        gtk_drop_down_set_selected (GTK_DROP_DOWN (self->mode_dd), i);
        return;
      }
}

static void
select_band_string (LogflWindow *self, const char *band)
{
  if (!band)
    return;
  for (guint i = 0; bands[i]; i++)
    if (g_str_equal (bands[i], band))
      {
        gtk_drop_down_set_selected (GTK_DROP_DOWN (self->band_dd), i);
        return;
      }
}

typedef struct {
  LogflWindow  *self;
  LogflTciState st;
} TciStateIdle;

static gboolean
tci_apply_state (gpointer user_data)
{
  TciStateIdle *d = user_data;
  LogflWindow *self = d->self;
  const LogflTciState *st = &d->st;

  /* Drop if the window is already tearing down. */
  if (self->tci_label == NULL)
    {
      g_free (d);
      return G_SOURCE_REMOVE;
    }

  /* Always refresh the status line; do not stomp the entry while editing. */
  if (!self->editing)
    {
      self->syncing_tci = TRUE;

      if (st->vfo_hz > 0)
        {
          char *mhz = fmt_freq (st->vfo_hz / 1e6);
          gtk_editable_set_text (GTK_EDITABLE (self->freq), mhz);
          g_free (mhz);
        }

      const char *log_mode = logfl_tci_mode_to_log (st->mode);
      if (log_mode)
        select_mode_string (self, log_mode);

      self->syncing_tci = FALSE;
      update_wb4 (self);
    }

  char *mhz_txt = st->vfo_hz > 0 ? fmt_freq (st->vfo_hz / 1e6) : g_strdup ("—");
  char *status = g_strdup_printf (
      "TCI · %s · %s MHz · %s",
      st->device[0] ? st->device : "radio",
      mhz_txt,
      st->mode[0] ? st->mode : "—");
  tci_set_status (self, status);
  g_free (status);
  g_free (mhz_txt);

  g_free (d);
  return G_SOURCE_REMOVE;
}

/* LWS thread → main loop. user_data is the window (stable while client lives). */
static void
on_tci_state (const LogflTciState *st, gpointer user_data)
{
  LogflWindow *self = user_data;
  TciStateIdle *d = g_new (TciStateIdle, 1);
  d->self = self;
  d->st = *st;
  g_idle_add (tci_apply_state, d);
}

typedef struct {
  LogflWindow    *self;   /* strong ref held for the job */
  LogflTciClient *cli;    /* owned by job until success installs it */
  guint           epoch;  /* must match self->tci_epoch to install */
  gboolean        ok;
} TciConnectResult;

static gboolean
tci_mark_offline (gpointer user_data)
{
  LogflWindow *self = user_data;
  if (self->tci_label == NULL)
    return G_SOURCE_REMOVE;

  if (self->tci)
    {
      logfl_tci_client_set_state_cb (self->tci, NULL, NULL);
      logfl_tci_client_set_closed_cb (self->tci, NULL, NULL);
      logfl_tci_client_free (self->tci);
      self->tci = NULL;
    }
  tci_set_status (self, "TCI offline");
  tci_schedule_connect (self);
  return G_SOURCE_REMOVE;
}

static void
on_tci_closed (gpointer user_data)
{
  g_idle_add (tci_mark_offline, user_data);
}

static gboolean
tci_connect_done (gpointer user_data)
{
  TciConnectResult *r = user_data;
  LogflWindow *self = r->self;
  self->tci_connecting = FALSE;

  /* Window gone or disposing: drop the orphan client. */
  if (self->tci_label == NULL)
    {
      if (r->cli)
        {
          logfl_tci_client_set_state_cb (r->cli, NULL, NULL);
          logfl_tci_client_set_closed_cb (r->cli, NULL, NULL);
          logfl_tci_client_free (r->cli);
        }
      g_object_unref (self);
      g_free (r);
      return G_SOURCE_REMOVE;
    }

  /* Stale job (prefs changed host/port mid-connect): drop and retry. */
  if (r->epoch != self->tci_epoch)
    {
      if (r->cli)
        {
          logfl_tci_client_set_state_cb (r->cli, NULL, NULL);
          logfl_tci_client_set_closed_cb (r->cli, NULL, NULL);
          logfl_tci_client_free (r->cli);
          r->cli = NULL;
        }
      if (!self->tci)
        tci_connect_kick (self);
      g_object_unref (self);
      g_free (r);
      return G_SOURCE_REMOVE;
    }

  if (r->ok && r->cli)
    {
      if (self->tci && self->tci != r->cli)
        {
          logfl_tci_client_set_state_cb (self->tci, NULL, NULL);
          logfl_tci_client_set_closed_cb (self->tci, NULL, NULL);
          logfl_tci_client_free (self->tci);
        }
      self->tci = r->cli;
      r->cli = NULL;
      g_clear_handle_id (&self->tci_retry_id, g_source_remove);
    }
  else
    {
      if (r->cli)
        {
          logfl_tci_client_free (r->cli);
          r->cli = NULL;
        }
      tci_set_status (self, "TCI offline");
      tci_schedule_connect (self);
    }

  g_object_unref (self);
  g_free (r);
  return G_SOURCE_REMOVE;
}

static gpointer
tci_connect_thread (gpointer data)
{
  TciConnectResult *r = data;
  GError *err = NULL;
  r->ok = logfl_tci_client_start (r->cli, &err);
  g_clear_error (&err);
  g_idle_add (tci_connect_done, r);
  return NULL;
}

static gboolean
tci_connect_kick (gpointer user_data)
{
  LogflWindow *self = user_data;
  self->tci_retry_id = 0;
  if (self->tci_label == NULL || self->tci_connecting)
    return G_SOURCE_REMOVE;
  if (self->tci && logfl_tci_client_is_ready (self->tci))
    return G_SOURCE_REMOVE;

  const char *host = self->settings.tci_host && *self->settings.tci_host
                         ? self->settings.tci_host
                         : LOGFL_TCI_DEFAULT_HOST;
  guint16 port = self->settings.tci_port
                     ? self->settings.tci_port
                     : LOGFL_TCI_DEFAULT_PORT;
  LogflTciClient *cli = logfl_tci_client_new (host, port);
  logfl_tci_client_set_state_cb (cli, on_tci_state, self);
  logfl_tci_client_set_closed_cb (cli, on_tci_closed, self);

  TciConnectResult *r = g_new0 (TciConnectResult, 1);
  r->self = g_object_ref (self);
  r->cli = cli;
  r->epoch = self->tci_epoch;
  self->tci_connecting = TRUE;
  tci_set_status (self, "TCI connecting…");
  g_thread_unref (g_thread_new ("logfl-tci-conn", tci_connect_thread, r));
  return G_SOURCE_REMOVE;
}

static void
tci_schedule_connect (LogflWindow *self)
{
  if (self->tci_connecting || self->tci_retry_id || self->tci_label == NULL)
    return;
  self->tci_retry_id =
      g_timeout_add_seconds (TCI_RETRY_S, tci_connect_kick, self);
}

static void
on_row_activate (GtkColumnView *view, guint position, gpointer user_data)
{
  (void) view;
  LogflWindow *self = user_data;
  if (!self->tci || !logfl_tci_client_is_ready (self->tci))
    {
      toast (self, "TCI not connected — cannot QSY");
      return;
    }
  LogflQsoRow *row =
      g_list_model_get_item (G_LIST_MODEL (self->rows), position);
  if (!row)
    return;
  const LogflQso *q = logfl_qso_row_qso (row);
  if (q->freq <= 0)
    {
      toast (self, "No frequency on this QSO");
      g_object_unref (row);
      return;
    }
  double hz = q->freq * 1e6;
  logfl_tci_client_tune (self->tci, hz);
  char *mhz = fmt_freq (q->freq);
  toast (self, "QSY %s → %s MHz", q->call, mhz);
  g_free (mhz);
  g_object_unref (row);
}

/* --- entry row logic ---------------------------------------------------- */

static void
update_wb4 (LogflWindow *self)
{
  const char *call = entry_text (self->call);
  GtkWidget *l = self->wb4_label;

  gtk_widget_remove_css_class (l, "success");
  gtk_widget_remove_css_class (l, "warning");
  if (!self->store || strlen (call) < 2)
    {
      gtk_label_set_text (GTK_LABEL (l), "");
      return;
    }

  LogflWorkedB4 wb;
  if (!logfl_store_worked_b4 (self->store, call,
                              dd_selected (self->band_dd, bands),
                              dd_selected (self->mode_dd, modes), &wb, NULL))
    return;
  if (wb.n_total == 0)
    {
      gtk_label_set_text (GTK_LABEL (l), "New call");
      gtk_widget_add_css_class (l, "success");
      return;
    }
  GDateTime *dt = g_date_time_new_from_unix_utc (wb.last_ts);
  char *when = g_date_time_format (dt, "%d.%m.%Y");
  char *txt = g_strdup_printf (
      "B4: %u× · this band %u× · band+mode %u× · last %s",
      wb.n_total, wb.n_band, wb.n_band_mode, when);
  gtk_label_set_text (GTK_LABEL (l), txt);
  gtk_widget_add_css_class (l, "warning");
  g_free (txt);
  g_free (when);
  g_date_time_unref (dt);
}

static const char *
rst_default_for_mode (const char *mode)
{
  if (g_strcmp0 (mode, "SSB") == 0 || g_strcmp0 (mode, "FM") == 0 ||
      g_strcmp0 (mode, "AM") == 0)
    return "59";
  /* Digital modes use SNR reports in practice; leave RST blank for manual
   * entry rather than inventing a CW-style 599. */
  if (g_strcmp0 (mode, "FT8") == 0 || g_strcmp0 (mode, "FT4") == 0)
    return "";
  return "599";                /* CW, RTTY, PSK31, … */
}

static gboolean
rst_looks_default (const char *s)
{
  return !*s || g_str_equal (s, "59") || g_str_equal (s, "599");
}

static void
on_mode_changed (LogflWindow *self)
{
  /* Refresh the RST defaults, but never stomp a hand-edited report. */
  const char *mode = dd_selected (self->mode_dd, modes);
  const char *def = rst_default_for_mode (mode);
  if (rst_looks_default (entry_text (self->rst_s)))
    gtk_editable_set_text (GTK_EDITABLE (self->rst_s), def);
  if (rst_looks_default (entry_text (self->rst_r)))
    gtk_editable_set_text (GTK_EDITABLE (self->rst_r), def);
  update_wb4 (self);
}

static void
on_freq_changed (LogflWindow *self)
{
  if (self->syncing_freq)
    return;
  /* Still apply band sync when the change came from TCI. */
  char *txt = g_strdup (entry_text (self->freq));
  g_strdelimit (txt, ",", '.');
  double mhz = g_ascii_strtod (txt, NULL);
  g_free (txt);
  const char *band = mhz > 0 ? logfl_adif_band_for_freq (mhz) : NULL;
  if (!band)
    return;
  for (guint i = 0; bands[i]; i++)
    if (g_str_equal (bands[i], band))
      {
        self->syncing_freq = TRUE;
        gtk_drop_down_set_selected (GTK_DROP_DOWN (self->band_dd), i);
        self->syncing_freq = FALSE;
        break;
      }
}

/* When the operator picks a band with an empty MHz field, seed a mid-band
 * frequency so logging does not store NULL (exact VFO still comes from TCI
 * or a typed value). */
static void
on_band_changed (LogflWindow *self)
{
  if (self->syncing_freq || self->syncing_tci)
    {
      update_wb4 (self);
      return;
    }
  const char *cur = entry_text (self->freq);
  if (cur && *cur)
    {
      update_wb4 (self);
      return;
    }
  const char *band = dd_selected (self->band_dd, bands);
  double mhz = logfl_adif_freq_for_band (band);
  if (mhz > 0)
    {
      char *txt = fmt_freq (mhz);
      self->syncing_freq = TRUE;
      gtk_editable_set_text (GTK_EDITABLE (self->freq), txt);
      self->syncing_freq = FALSE;
      g_free (txt);
    }
  update_wb4 (self);
}

static void
clear_entry_row (LogflWindow *self)
{
  gtk_editable_set_text (GTK_EDITABLE (self->call), "");
  gtk_editable_set_text (GTK_EDITABLE (self->name), "");
  gtk_editable_set_text (GTK_EDITABLE (self->comment), "");
  gtk_label_set_text (GTK_LABEL (self->wb4_label), "");
  gtk_widget_grab_focus (self->call);
}

static void
set_edit_ui (LogflWindow *self, gboolean on)
{
  if (self->log_btn)
    gtk_button_set_label (GTK_BUTTON (self->log_btn),
                          on ? "Save QSO" : "Log QSO");
  if (self->cancel_edit_btn)
    gtk_widget_set_visible (self->cancel_edit_btn, on);
}

static void
exit_edit_mode (LogflWindow *self)
{
  g_clear_pointer (&self->editing, logfl_qso_free);
  set_edit_ui (self, FALSE);
}

/* Fill the entry row from a stored QSO (edit mode). Band/mode/freq guards
 * avoid mid-band seeding and TCI feedback while we load. */
static void
load_qso_into_entry (LogflWindow *self, const LogflQso *q)
{
  self->syncing_tci = TRUE;
  self->syncing_freq = TRUE;

  gtk_editable_set_text (GTK_EDITABLE (self->call),
                         q->call ? q->call : "");
  gtk_editable_set_text (GTK_EDITABLE (self->rst_s),
                         q->rst_sent ? q->rst_sent : "");
  gtk_editable_set_text (GTK_EDITABLE (self->rst_r),
                         q->rst_rcvd ? q->rst_rcvd : "");
  select_band_string (self, q->band);
  select_mode_string (self, q->mode);
  if (q->freq > 0)
    {
      char *txt = fmt_freq (q->freq);
      gtk_editable_set_text (GTK_EDITABLE (self->freq), txt);
      g_free (txt);
    }
  else
    gtk_editable_set_text (GTK_EDITABLE (self->freq), "");
  gtk_editable_set_text (GTK_EDITABLE (self->name),
                         q->name ? q->name : "");
  gtk_editable_set_text (GTK_EDITABLE (self->comment),
                         q->comment ? q->comment : "");

  self->syncing_tci = FALSE;
  self->syncing_freq = FALSE;
  update_wb4 (self);
}

static void
begin_edit_qso (LogflWindow *self, gint64 id)
{
  if (!self->store)
    {
      toast (self, "Log store is not open");
      return;
    }
  if (self->pending)
    {
      toast (self, "Confirm the previous QSO first");
      return;
    }

  GError *err = NULL;
  LogflQso *q = logfl_store_get (self->store, id, &err);
  if (!q)
    {
      toast (self, "Cannot edit: %s",
             err ? err->message : "not found");
      g_clear_error (&err);
      return;
    }

  g_clear_pointer (&self->editing, logfl_qso_free);
  self->editing = q;
  load_qso_into_entry (self, q);
  set_edit_ui (self, TRUE);
  gtk_window_present (GTK_WINDOW (self));
  gtk_widget_grab_focus (self->call);
  toast (self, "Editing %s — Save or Cancel",
         q->call ? q->call : "?");
}

static void
cancel_edit_qso (LogflWindow *self)
{
  if (!self->editing)
    return;
  exit_edit_mode (self);
  clear_entry_row (self);
  toast (self, "Edit cancelled");
}

/* Resolve MHz for a new log (not used for edit — empty stays unset so the
 * operator can fill a missing freq from before the mid-band fix). */
static double
resolve_log_freq (LogflWindow *self, const char *band)
{
  char *ftxt = g_strdup (entry_text (self->freq));
  g_strdelimit (ftxt, ",", '.');
  double mhz = g_ascii_strtod (ftxt, NULL);
  g_free (ftxt);
  if (mhz <= 0 && self->tci && logfl_tci_client_is_ready (self->tci))
    {
      LogflTciState st;
      logfl_tci_client_get_state (self->tci, &st);
      if (st.vfo_hz > 0)
        mhz = st.vfo_hz / 1e6;
    }
  if (mhz <= 0 && band)
    mhz = logfl_adif_freq_for_band (band);
  return mhz;
}

/* Apply editable entry fields onto q; leave id/ts/extras/QSL/etc. alone. */
static void
apply_entry_to_qso (LogflWindow *self, LogflQso *q, gboolean is_new)
{
  g_free (q->call);
  g_free (q->band);
  g_free (q->mode);
  g_free (q->rst_sent);
  g_free (q->rst_rcvd);
  g_free (q->name);
  g_free (q->comment);

  q->call = g_strdup (entry_text (self->call));
  q->band = g_strdup (dd_selected (self->band_dd, bands));
  q->mode = g_strdup (dd_selected (self->mode_dd, modes));
  q->rst_sent = g_strdup (entry_text (self->rst_s));
  q->rst_rcvd = g_strdup (entry_text (self->rst_r));
  q->name = g_strdup (entry_text (self->name));
  q->comment = g_strdup (entry_text (self->comment));

  char *ftxt = g_strdup (entry_text (self->freq));
  g_strdelimit (ftxt, ",", '.');
  double mhz = g_ascii_strtod (ftxt, NULL);
  g_free (ftxt);

  if (is_new)
    {
      q->ts = g_get_real_time () / G_USEC_PER_SEC;
      q->freq = resolve_log_freq (self, q->band);
      g_free (q->station_callsign);
      q->station_callsign = g_strdup (
          self->settings.station_callsign && *self->settings.station_callsign
              ? self->settings.station_callsign
              : "OK1BR");
    }
  else
    {
      /* Edit: typed MHz wins; empty keeps previous (or 0 if it was NULL).
       * Band mid-point only when both empty and still unset — so old
       * band-only rows can be fixed by typing MHz without force-seeding. */
      if (mhz > 0)
        q->freq = mhz;
      else if (q->freq <= 0 && q->band)
        q->freq = logfl_adif_freq_for_band (q->band);
    }
}

static void
do_save_edit (LogflWindow *self)
{
  LogflQso *q = self->editing;
  if (!q)
    return;

  const char *call = entry_text (self->call);
  if (!call || !*call)
    {
      toast (self, "Callsign first");
      gtk_widget_grab_focus (self->call);
      return;
    }

  apply_entry_to_qso (self, q, FALSE);

  GError *err = NULL;
  if (logfl_store_update (self->store, q, &err))
    {
      toast (self, "Updated %s · %s · %s", q->call, q->band, q->mode);
      exit_edit_mode (self);
      clear_entry_row (self);
      reload (self);
    }
  else
    {
      toast (self, "Not saved: %s", err->message);
      g_clear_error (&err);
    }
}

static void
do_add_pending (LogflWindow *self)
{
  GError *err = NULL;
  LogflQso *q = g_steal_pointer (&self->pending);
  if (logfl_store_add (self->store, q, &err))
    {
      toast (self, "Logged %s · %s · %s", q->call, q->band, q->mode);
      if (self->settings.esm_enabled)
        self->esm_phase = LOGFL_ESM_PHASE_TU;
      clear_entry_row (self);
      reload (self);
      refresh_esm_hint (self);
    }
  else
    {
      toast (self, "Not logged: %s", err->message);
      g_clear_error (&err);
    }
  logfl_qso_free (q);
}

static void
on_dup_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LogflWindow *self = user_data;
  const char *resp =
      adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), res);
  if (g_str_equal (resp, "log"))
    do_add_pending (self);
  else
    g_clear_pointer (&self->pending, logfl_qso_free);
}

static void
log_qso (LogflWindow *self)
{
  if (!self->store)
    {
      toast (self, "Log store is not open");
      return;
    }

  /* Edit path: update in place; no dup dialog (same row). */
  if (self->editing)
    {
      do_save_edit (self);
      return;
    }

  /* One pending confirm at a time — replacing pending would free the QSO
   * still owned by an open dialog. */
  if (self->pending)
    {
      toast (self, "Confirm the previous QSO first");
      return;
    }

  const char *call = entry_text (self->call);
  if (!*call)
    {
      toast (self, "Callsign first");
      gtk_widget_grab_focus (self->call);
      return;
    }

  LogflQso *q = logfl_qso_new ();
  apply_entry_to_qso (self, q, TRUE);

  gboolean dup = FALSE;
  logfl_store_dup_check (self->store, q->call, q->band, q->mode, q->ts,
                         DUP_WINDOW_S, &dup, NULL);
  self->pending = q;
  if (!dup)
    {
      do_add_pending (self);
      return;
    }

  AdwDialog *dlg = adw_alert_dialog_new ("Duplicate?", NULL);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (dlg),
      "%s was already logged on %s/%s within ±%d min.",
      q->call, q->band, q->mode, DUP_WINDOW_S / 60);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", "Cancel", "log", "Log anyway",
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "log",
                                            ADW_RESPONSE_SUGGESTED);
  /* Prefer Cancel on Enter so an accidental duplicate is not logged. */
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg), GTK_WIDGET (self), NULL,
                           on_dup_response, self);
}

/* --- macros v2 (editable banks, Run/S&P, ESM) --------------------------- */

static void refresh_macro_bar (LogflWindow *self);
static void refresh_esm_hint (LogflWindow *self);

static const LogflMacroKey *
active_macro_key (LogflWindow *self, guint idx)
{
  return logfl_macro_set_key (&self->settings.macros,
                              self->settings.macro_bank, idx);
}

/* Human label for a slot: F1–F12, free M1…, or STOP. */
static char *
macro_slot_name (guint idx)
{
  if (logfl_macro_index_is_stop (idx))
    return g_strdup ("STOP");
  if (idx < LOGFL_MACRO_N_ROW)
    return g_strdup_printf ("F%u", idx + 1);
  return g_strdup_printf ("M%u", idx - LOGFL_MACRO_N_ROW + 1);
}

static void
macro_run (LogflWindow *self, guint idx)
{
  if (idx >= LOGFL_MACRO_N_KEYS)
    return;

  /* Dedicated stop slot (last of row 2) — also Esc. Empty free keys are not stop. */
  if (logfl_macro_index_is_stop (idx))
    {
      if (self->tci && logfl_tci_client_is_ready (self->tci))
        {
          logfl_tci_client_cw_stop (self->tci);
          toast (self, "CW stop");
        }
      else
        toast (self, "TCI not connected");
      return;
    }

  const LogflMacroKey *k = active_macro_key (self, idx);
  if (!k)
    return;

  const char *mycall =
      self->settings.station_callsign && *self->settings.station_callsign
          ? self->settings.station_callsign
          : "OK1BR";
  char *msg = logfl_macro_expand (k->tmpl, mycall, entry_text (self->call),
                                  entry_text (self->rst_s));
  if (!msg || !*msg)
    {
      g_free (msg);
      char *slot = macro_slot_name (idx);
      toast (self, "Empty macro %s (edit or need a callsign?)", slot);
      g_free (slot);
      return;
    }

  char *slot = macro_slot_name (idx);
  const char *cap = k->caption && *k->caption ? k->caption : slot;

  if (!self->tci || !logfl_tci_client_is_ready (self->tci))
    {
      toast (self, "TCI offline — %s: %s", cap, msg);
      g_free (msg);
      g_free (slot);
      return;
    }

  /* CW path only (SSB wav/DVK out of scope for M5). */
  const char *mode = dd_selected (self->mode_dd, modes);
  if (mode && g_strcmp0 (mode, "CW") != 0 && g_strcmp0 (mode, "RTTY") != 0)
    {
      toast (self, "Macros send CW via TCI — switch mode to CW (%s)", msg);
      g_free (msg);
      g_free (slot);
      return;
    }

  logfl_tci_client_cw_send (self->tci, msg);
  toast (self, "TX %s: %s", cap, msg);
  g_free (msg);
  g_free (slot);
}

static void
on_macro_clicked (GtkButton *btn, gpointer user_data)
{
  LogflWindow *self = user_data;
  guint idx = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (btn), "macro"));
  macro_run (self, idx);
}

/* Right-click a macro key → edit caption + template for the active bank. */
static void
on_macro_edit_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LogflWindow *self = user_data;
  AdwAlertDialog *dlg = ADW_ALERT_DIALOG (source);
  const char *resp = adw_alert_dialog_choose_finish (dlg, res);
  guint idx = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (dlg), "macro"));
  GtkWidget *cap_e = g_object_get_data (G_OBJECT (dlg), "cap");
  GtkWidget *tmpl_e = g_object_get_data (G_OBJECT (dlg), "tmpl");
  if (!g_str_equal (resp, "save") || !cap_e || !tmpl_e)
    return;

  logfl_macro_set_set_key (&self->settings.macros, self->settings.macro_bank,
                           idx, gtk_editable_get_text (GTK_EDITABLE (cap_e)),
                           gtk_editable_get_text (GTK_EDITABLE (tmpl_e)));
  logfl_settings_save (&self->settings);
  refresh_macro_bar (self);
  char *slot = macro_slot_name (idx);
  toast (self, "Macro %s saved", slot);
  g_free (slot);
}

static void
macro_edit_dialog (LogflWindow *self, guint idx)
{
  const LogflMacroKey *k = active_macro_key (self, idx);
  if (!k)
    return;

  const char *bank =
      self->settings.macro_bank == LOGFL_MACRO_BANK_SNP ? "S&P" : "Run";
  char *slot = macro_slot_name (idx);
  char *title = g_strdup_printf ("Edit %s · %s", slot, bank);
  AdwDialog *dlg = adw_alert_dialog_new (title, NULL);
  g_free (title);
  g_free (slot);
  if (logfl_macro_index_is_stop (idx))
    adw_alert_dialog_format_body (
        ADW_ALERT_DIALOG (dlg),
        "STOP slot — Esc always stops the keyer. Caption is label only; "
        "template is ignored.");
  else
    adw_alert_dialog_format_body (
        ADW_ALERT_DIALOG (dlg),
        "Tokens: {MYCALL} {CALL} {RST}  ·  empty text = unused free slot");

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *cap_e = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (cap_e), "Caption");
  gtk_editable_set_text (GTK_EDITABLE (cap_e),
                         k->caption ? k->caption : "");
  GtkWidget *tmpl_e = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (tmpl_e), "CW text");
  gtk_editable_set_text (GTK_EDITABLE (tmpl_e), k->tmpl ? k->tmpl : "");
  gtk_box_append (GTK_BOX (box), labeled ("Caption", cap_e));
  gtk_box_append (GTK_BOX (box), labeled ("Template", tmpl_e));
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dlg), box);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", "Cancel", "save", "Save", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "save",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "save");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");

  g_object_set_data (G_OBJECT (dlg), "macro", GUINT_TO_POINTER (idx));
  g_object_set_data (G_OBJECT (dlg), "cap", cap_e);
  g_object_set_data (G_OBJECT (dlg), "tmpl", tmpl_e);
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg), GTK_WIDGET (self), NULL,
                           on_macro_edit_response, self);
}

static void
on_macro_right_click (GtkGestureClick *gesture, gint n_press, gdouble x,
                      gdouble y, gpointer user_data)
{
  (void) n_press;
  (void) x;
  (void) y;
  LogflWindow *self = user_data;
  GtkWidget *btn = gtk_event_controller_get_widget (
      GTK_EVENT_CONTROLLER (gesture));
  guint idx = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (btn), "macro"));
  macro_edit_dialog (self, idx);
}

static void
set_macro_bank (LogflWindow *self, LogflMacroBankId bank)
{
  if (self->settings.macro_bank == bank)
    {
      /* Keep toggle visuals consistent. */
      if (self->bank_run_btn)
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->bank_run_btn),
                                      bank == LOGFL_MACRO_BANK_RUN);
      if (self->bank_snp_btn)
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->bank_snp_btn),
                                      bank == LOGFL_MACRO_BANK_SNP);
      return;
    }
  self->settings.macro_bank = bank;
  logfl_settings_save (&self->settings);
  refresh_macro_bar (self);
  if (self->bank_run_btn)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->bank_run_btn),
                                  bank == LOGFL_MACRO_BANK_RUN);
  if (self->bank_snp_btn)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->bank_snp_btn),
                                  bank == LOGFL_MACRO_BANK_SNP);
}

static void
on_bank_run_toggled (GtkToggleButton *btn, gpointer user_data)
{
  LogflWindow *self = user_data;
  if (gtk_toggle_button_get_active (btn))
    set_macro_bank (self, LOGFL_MACRO_BANK_RUN);
}

static void
on_bank_snp_toggled (GtkToggleButton *btn, gpointer user_data)
{
  LogflWindow *self = user_data;
  if (gtk_toggle_button_get_active (btn))
    set_macro_bank (self, LOGFL_MACRO_BANK_SNP);
}

static void
refresh_macro_bar (LogflWindow *self)
{
  for (guint i = 0; i < LOGFL_MACRO_N_KEYS; i++)
    {
      GtkWidget *btn = self->macro_btns[i];
      if (!btn)
        continue;
      const LogflMacroKey *k = active_macro_key (self, i);
      const char *cap = (k && k->caption && *k->caption) ? k->caption : "—";
      char *lab;
      char *tip;

      if (logfl_macro_index_is_stop (i))
        {
          /* Two-line label like the others so row height matches. */
          lab = g_strdup ("STOP\nEsc");
          tip = g_strdup ("Esc · stop CW keyer\nRight-click to edit label");
        }
      else if (i < LOGFL_MACRO_N_ROW)
        {
          lab = g_strdup_printf ("F%u\n%s", i + 1, cap);
          tip = g_strdup_printf ("F%u · %s\nRight-click to edit", i + 1,
                                 (k && k->tmpl) ? k->tmpl : "");
        }
      else
        {
          /* Second row free slots M1… (last is STOP, handled above). */
          guint m = i - LOGFL_MACRO_N_ROW + 1;
          lab = g_strdup_printf ("M%u\n%s", m, cap);
          tip = g_strdup_printf ("Free M%u · %s\nRight-click to edit", m,
                                 (k && k->tmpl) ? k->tmpl : "");
        }
      gtk_button_set_label (GTK_BUTTON (btn), lab);
      gtk_widget_set_tooltip_text (btn, tip);
      g_free (lab);
      g_free (tip);
    }
}

static void
refresh_esm_hint (LogflWindow *self)
{
  if (!self->esm_hint)
    return;
  if (!self->settings.esm_enabled)
    {
      gtk_label_set_text (GTK_LABEL (self->esm_hint), "");
      return;
    }
  const char *ph = "Ready";
  switch (self->esm_phase)
    {
    case LOGFL_ESM_PHASE_LOG:
      ph = "Enter → Log QSO";
      break;
    case LOGFL_ESM_PHASE_TU:
      ph = "Enter → TU";
      break;
    case LOGFL_ESM_PHASE_READY:
    default:
      ph = "Enter → CQ / EXCH";
      break;
    }
  char *txt = g_strdup_printf ("ESM on · %s", ph);
  gtk_label_set_text (GTK_LABEL (self->esm_hint), txt);
  g_free (txt);
}

static void
esm_enter (LogflWindow *self)
{
  const char *call = entry_text (self->call);
  gboolean present = call && *call;
  guint key = 0;
  LogflEsmAct act = logfl_esm_decide (self->esm_phase, present, &key);

  switch (act)
    {
    case LOGFL_ESM_ACT_SEND_MACRO:
      macro_run (self, key);
      self->esm_phase =
          logfl_esm_next (self->esm_phase, act, present);
      refresh_esm_hint (self);
      break;
    case LOGFL_ESM_ACT_LOG:
      self->esm_force_log = TRUE;
      log_qso (self);
      self->esm_force_log = FALSE;
      /* Phase advances in do_add_pending on success; if still pending
       * (dup dialog), leave LOG so a cancelled dup can retry. */
      refresh_esm_hint (self);
      break;
    case LOGFL_ESM_ACT_NONE:
    default:
      break;
    }
}

/* Enter in entry fields: ESM cycle when enabled, else log (or save edit). */
static void
on_entry_activate (LogflWindow *self)
{
  if (self->editing || self->esm_force_log || !self->settings.esm_enabled)
    {
      log_qso (self);
      return;
    }
  esm_enter (self);
}

static gboolean
on_main_key (GtkEventControllerKey *ctl, guint keyval, guint keycode,
             GdkModifierType state, gpointer user_data)
{
  (void) ctl;
  (void) keycode;
  LogflWindow *self = user_data;
  if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
    return FALSE;
  if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F8)
    {
      macro_run (self, keyval - GDK_KEY_F1);
      return TRUE;
    }
  if (keyval == GDK_KEY_Escape)
    {
      macro_run (self, LOGFL_MACRO_STOP_IDX);
      return TRUE;
    }
  return FALSE;
}

/* Linked Run / S&P toggles for the header bar (between log and menu). */
static GtkWidget *
build_bank_header_toggle (LogflWindow *self)
{
  self->bank_run_btn = gtk_toggle_button_new_with_label ("Run");
  self->bank_snp_btn = gtk_toggle_button_new_with_label ("S&P");
  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (self->bank_snp_btn),
                               GTK_TOGGLE_BUTTON (self->bank_run_btn));
  gtk_toggle_button_set_active (
      GTK_TOGGLE_BUTTON (self->bank_run_btn),
      self->settings.macro_bank == LOGFL_MACRO_BANK_RUN);
  gtk_toggle_button_set_active (
      GTK_TOGGLE_BUTTON (self->bank_snp_btn),
      self->settings.macro_bank == LOGFL_MACRO_BANK_SNP);
  gtk_widget_set_tooltip_text (self->bank_run_btn,
                               "Run message bank (F1–F8 + free row)");
  gtk_widget_set_tooltip_text (self->bank_snp_btn,
                               "Search & Pounce message bank (F1–F8 + free row)");
  g_signal_connect (self->bank_run_btn, "toggled",
                    G_CALLBACK (on_bank_run_toggled), self);
  g_signal_connect (self->bank_snp_btn, "toggled",
                    G_CALLBACK (on_bank_snp_toggled), self);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (box, "linked");
  gtk_box_append (GTK_BOX (box), self->bank_run_btn);
  gtk_box_append (GTK_BOX (box), self->bank_snp_btn);
  return box;
}

static GtkWidget *
build_macro_bar (LogflWindow *self)
{
  GtkWidget *wrap = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);

  self->esm_hint = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->esm_hint), 0);
  gtk_widget_add_css_class (self->esm_hint, "dim-label");
  gtk_widget_add_css_class (self->esm_hint, "caption");
  gtk_box_append (GTK_BOX (wrap), self->esm_hint);

  /* Two rows of 8: F1–F8, then free M1–M7 + STOP. Homogeneous so every
   * key has the same width; STOP is a normal button (not destructive red). */
  for (guint row = 0; row < LOGFL_MACRO_N_ROWS; row++)
    {
      GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
      gtk_widget_set_hexpand (bar, TRUE);
      gtk_box_set_homogeneous (GTK_BOX (bar), TRUE);
      for (guint col = 0; col < LOGFL_MACRO_N_ROW; col++)
        {
          guint i = row * LOGFL_MACRO_N_ROW + col;
          GtkWidget *btn = gtk_button_new_with_label ("·");
          gtk_widget_set_hexpand (btn, TRUE);
          gtk_widget_set_halign (btn, GTK_ALIGN_FILL);
          gtk_widget_set_focus_on_click (btn, FALSE);
          g_object_set_data (G_OBJECT (btn), "macro", GUINT_TO_POINTER (i));
          g_signal_connect (btn, "clicked", G_CALLBACK (on_macro_clicked),
                            self);

          GtkGesture *rg = gtk_gesture_click_new ();
          gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rg),
                                         GDK_BUTTON_SECONDARY);
          g_signal_connect (rg, "pressed",
                            G_CALLBACK (on_macro_right_click), self);
          gtk_widget_add_controller (btn, GTK_EVENT_CONTROLLER (rg));

          self->macro_btns[i] = btn;
          gtk_box_append (GTK_BOX (bar), btn);
        }
      gtk_box_append (GTK_BOX (wrap), bar);
    }

  refresh_macro_bar (self);
  refresh_esm_hint (self);
  return wrap;
}

static gboolean
clock_tick (gpointer user_data)
{
  LogflWindow *self = user_data;
  GDateTime *now = g_date_time_new_now_utc ();
  char *txt = g_date_time_format (now, "%H:%M:%S UTC · %d.%m.%Y");
  gtk_label_set_text (GTK_LABEL (self->clock_label), txt);
  g_free (txt);
  g_date_time_unref (now);
  return G_SOURCE_CONTINUE;
}

/* --- delete ------------------------------------------------------------- */

static void
on_delete_response (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LogflWindow *self = user_data;
  const char *resp =
      adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), res);
  self->delete_confirm_open = FALSE;
  if (!g_str_equal (resp, "delete"))
    {
      self->pending_delete_id = 0;
      return;
    }
  GError *err = NULL;
  gint64 deleted_id = self->pending_delete_id;
  if (logfl_store_delete (self->store, deleted_id, &err))
    {
      toast (self, "QSO deleted");
      if (self->editing && self->editing->id == deleted_id)
        {
          exit_edit_mode (self);
          clear_entry_row (self);
        }
      reload (self);
    }
  else
    {
      toast (self, "Delete failed: %s", err->message);
      g_clear_error (&err);
    }
  self->pending_delete_id = 0;
}

static void
on_delete_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  LogflWindow *self = user_data;
  if (self->delete_confirm_open)
    return;
  LogflQsoRow *row = gtk_single_selection_get_selected_item (self->selection);
  if (!row)
    return;
  const LogflQso *q = logfl_qso_row_qso (row);
  self->pending_delete_id = q->id;
  self->delete_confirm_open = TRUE;

  GDateTime *dt = g_date_time_new_from_unix_utc (q->ts);
  char *when = g_date_time_format (dt, "%d.%m.%Y %H:%M");
  AdwDialog *dlg = adw_alert_dialog_new ("Delete QSO?", NULL);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (dlg),
      "Delete %s · %s · %s · %s UTC? This cannot be undone.",
      q->call, q->band, q->mode, when);
  g_free (when);
  g_date_time_unref (dt);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", "Cancel", "delete", "Delete",
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "delete",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg), GTK_WIDGET (self), NULL,
                           on_delete_response, self);
}

static void
on_edit_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  LogflWindow *self = user_data;
  LogflQsoRow *row = gtk_single_selection_get_selected_item (self->selection);
  if (!row)
    {
      toast (self, "Select a QSO to edit");
      return;
    }
  const LogflQso *q = logfl_qso_row_qso (row);
  begin_edit_qso (self, q->id);
}

/* --- ADIF import / export ---------------------------------------------- */

static void
on_import_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LogflWindow *self = user_data;
  GError *err = NULL;
  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res,
                                             &err);
  if (!file)
    {
      g_clear_error (&err);      /* dismissed */
      return;
    }
  /* g_file_get_path is NULL for non-native URIs (portals, remote); read via
   * GFile so import still works when the dialog does not yield a local path. */
  char *data = NULL;
  gsize len = 0;
  LogflAdifReport rep;
  if (!g_file_load_contents (file, NULL, &data, &len, NULL, &err))
    {
      toast (self, "Import failed: %s", err->message);
      g_clear_error (&err);
    }
  else if (!self->store)
    {
      toast (self, "Log store is not open");
    }
  else if (logfl_adif_import_data (self->store, data, (gssize) len,
                                   ADIF_IMPORT_DUP_WINDOW_S, &rep, &err))
    {
      toast (self, "Imported %u QSO · %u dups skipped · %u bad records",
             rep.n_imported, rep.n_dup_skipped, rep.n_bad);
      reload (self);
    }
  else
    {
      toast (self, "Import failed: %s", err->message);
      g_clear_error (&err);
    }
  g_free (data);
  g_object_unref (file);
}

static void
act_import (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void) action;
  (void) param;
  LogflWindow *self = user_data;
  GtkFileDialog *dlg = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dlg, "Import ADIF");
  gtk_file_dialog_open (dlg, GTK_WINDOW (self), NULL, on_import_ready, self);
  g_object_unref (dlg);
}

static void
on_export_ready (GObject *source, GAsyncResult *res, gpointer user_data)
{
  LogflWindow *self = user_data;
  GError *err = NULL;
  GFile *file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), res,
                                             &err);
  if (!file)
    {
      g_clear_error (&err);
      return;
    }
  if (!self->store)
    {
      toast (self, "Log store is not open");
      g_object_unref (file);
      return;
    }
  guint n = 0;
  char *data = logfl_adif_export_data (self->store, NULL, &n, &err);
  if (!data)
    {
      toast (self, "Export failed: %s", err->message);
      g_clear_error (&err);
      g_object_unref (file);
      return;
    }
  /* Same as import: write through GFile so non-native targets work. */
  if (g_file_replace_contents (file, data, strlen (data), NULL, FALSE,
                               G_FILE_CREATE_NONE, NULL, NULL, &err))
    {
      char *path = g_file_get_path (file);
      if (path)
        toast (self, "Exported %u QSO to %s", n, path);
      else
        toast (self, "Exported %u QSO", n);
      g_free (path);
    }
  else
    {
      toast (self, "Export failed: %s", err->message);
      g_clear_error (&err);
    }
  g_free (data);
  g_object_unref (file);
}

static void
act_export (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void) action;
  (void) param;
  LogflWindow *self = user_data;
  GtkFileDialog *dlg = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dlg, "Export ADIF");
  gtk_file_dialog_set_initial_name (dlg, "ok1br-log.adi");
  gtk_file_dialog_save (dlg, GTK_WINDOW (self), NULL, on_export_ready, self);
  g_object_unref (dlg);
}

/* --- preferences (skimmer/sdr house style) ------------------------------ */

static void
tci_disconnect (LogflWindow *self)
{
  g_clear_handle_id (&self->tci_retry_id, g_source_remove);
  if (self->tci)
    {
      logfl_tci_client_set_state_cb (self->tci, NULL, NULL);
      logfl_tci_client_set_closed_cb (self->tci, NULL, NULL);
      logfl_tci_client_free (self->tci);
      self->tci = NULL;
    }
  tci_set_status (self, "TCI offline");
}

static void
tci_reconnect_now (LogflWindow *self)
{
  /* Invalidate any in-flight connect job; it will free itself and re-kick. */
  self->tci_epoch++;
  tci_disconnect (self);
  if (!self->tci_connecting && self->tci_label)
    tci_connect_kick (self);
}

static void
prefs_closed (AdwDialog *dlg, gpointer user_data)
{
  LogflWindow *self = user_data;
  GtkWidget *host_row = g_object_get_data (G_OBJECT (dlg), "tci-host");
  GtkWidget *port_row = g_object_get_data (G_OBJECT (dlg), "tci-port");
  GtkWidget *call_row = g_object_get_data (G_OBJECT (dlg), "station-call");
  GtkWidget *esm_row = g_object_get_data (G_OBJECT (dlg), "esm");

  const char *h = gtk_editable_get_text (GTK_EDITABLE (host_row));
  char *host = g_strstrip (g_strdup ((h && *h) ? h : LOGFL_TCI_DEFAULT_HOST));
  int port = (int) adw_spin_row_get_value (ADW_SPIN_ROW (port_row));
  if (port < 1 || port > 65535)
    port = LOGFL_TCI_DEFAULT_PORT;
  const char *c = gtk_editable_get_text (GTK_EDITABLE (call_row));
  char *call = g_strstrip (g_strdup (c ? c : ""));
  gboolean esm = esm_row
                     ? adw_switch_row_get_active (ADW_SWITCH_ROW (esm_row))
                     : FALSE;

  gboolean tci_changed =
      g_strcmp0 (host, self->settings.tci_host) != 0 ||
      (guint16) port != self->settings.tci_port;
  gboolean call_changed =
      g_strcmp0 (call, self->settings.station_callsign) != 0;
  gboolean esm_changed = esm != self->settings.esm_enabled;

  if (tci_changed || call_changed || esm_changed)
    {
      g_free (self->settings.tci_host);
      self->settings.tci_host = host;
      self->settings.tci_port = (guint16) port;
      g_free (self->settings.station_callsign);
      self->settings.station_callsign = call;
      self->settings.esm_enabled = esm;
      if (esm_changed)
        self->esm_phase = LOGFL_ESM_PHASE_READY;
      logfl_settings_save (&self->settings);
      if (tci_changed)
        tci_reconnect_now (self);
      refresh_esm_hint (self);
    }
  else
    {
      g_free (host);
      g_free (call);
    }
}

static void
act_preferences (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void) action;
  (void) param;
  LogflWindow *self = user_data;

  /* Same pattern as sdr-for-linux: one AdwPreferencesPage per topic with
   * title + icon so the dialog shows a header page switcher (tabs), not a
   * single long scroll of mixed groups. */
  AdwDialog *dlg = adw_preferences_dialog_new ();
  adw_dialog_set_title (dlg, "Preferences");

  /* --- Station --------------------------------------------------------- */
  AdwPreferencesPage *p_station = ADW_PREFERENCES_PAGE (g_object_new (
      ADW_TYPE_PREFERENCES_PAGE,
      "title", "Station",
      "icon-name", "avatar-default-symbolic",
      NULL));
  AdwPreferencesGroup *sgrp = ADW_PREFERENCES_GROUP (g_object_new (
      ADW_TYPE_PREFERENCES_GROUP,
      "title", "Operator",
      "description", "Stamped on new QSOs as STATION_CALLSIGN (ADIF)",
      NULL));
  GtkWidget *call_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (call_row), "Callsign");
  gtk_editable_set_text (
      GTK_EDITABLE (call_row),
      self->settings.station_callsign ? self->settings.station_callsign : "");
  adw_preferences_group_add (sgrp, call_row);
  adw_preferences_page_add (p_station, sgrp);
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dlg), p_station);

  /* --- TCI (client → sdr-for-linux server) ----------------------------- */
  AdwPreferencesPage *p_tci = ADW_PREFERENCES_PAGE (g_object_new (
      ADW_TYPE_PREFERENCES_PAGE,
      "title", "TCI",
      "icon-name", "network-transmit-receive-symbolic",
      NULL));
  AdwPreferencesGroup *tgrp = ADW_PREFERENCES_GROUP (g_object_new (
      ADW_TYPE_PREFERENCES_GROUP,
      "title", "Server",
      "description",
      "sdr-for-linux WebSocket endpoint — connection is automatic; "
      "changing host or port reconnects immediately",
      NULL));
  GtkWidget *host_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (host_row), "Host");
  gtk_editable_set_text (
      GTK_EDITABLE (host_row),
      self->settings.tci_host ? self->settings.tci_host
                              : LOGFL_TCI_DEFAULT_HOST);
  adw_preferences_group_add (tgrp, host_row);

  GtkWidget *port_row = adw_spin_row_new_with_range (1, 65535, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (port_row), "Port");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (port_row),
                               "ExpertSDR / sdr-for-linux default 40001");
  adw_spin_row_set_value (
      ADW_SPIN_ROW (port_row),
      self->settings.tci_port ? self->settings.tci_port
                              : LOGFL_TCI_DEFAULT_PORT);
  adw_preferences_group_add (tgrp, port_row);
  adw_preferences_page_add (p_tci, tgrp);
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dlg), p_tci);

  /* --- Messaging (macros / ESM) ---------------------------------------- */
  AdwPreferencesPage *p_msg = ADW_PREFERENCES_PAGE (g_object_new (
      ADW_TYPE_PREFERENCES_PAGE,
      "title", "Messaging",
      "icon-name", "input-keyboard-symbolic",
      NULL));
  AdwPreferencesGroup *cgrp = ADW_PREFERENCES_GROUP (g_object_new (
      ADW_TYPE_PREFERENCES_GROUP,
      "title", "ESM",
      "description",
      "Enter advances CQ → exchange → log → TU. "
      "Off keeps Enter = Log QSO for daily use. "
      "Edit F-keys with right-click on the macro bar (Run / S&amp;P banks).",
      NULL));
  GtkWidget *esm_row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (esm_row),
                                 "Enter sends message");
  adw_switch_row_set_active (ADW_SWITCH_ROW (esm_row),
                             self->settings.esm_enabled);
  adw_preferences_group_add (cgrp, esm_row);
  adw_preferences_page_add (p_msg, cgrp);
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dlg), p_msg);

  /* Keep the page switcher in the header (sdr-for-linux does the same). */
  adw_dialog_set_content_width (dlg, 640);
  adw_dialog_set_content_height (dlg, 420);

  g_object_set_data (G_OBJECT (dlg), "tci-host", host_row);
  g_object_set_data (G_OBJECT (dlg), "tci-port", port_row);
  g_object_set_data (G_OBJECT (dlg), "station-call", call_row);
  g_object_set_data (G_OBJECT (dlg), "esm", esm_row);
  g_signal_connect (dlg, "closed", G_CALLBACK (prefs_closed), self);
  adw_dialog_present (dlg, GTK_WIDGET (self));
}

static void
act_about (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void) action;
  (void) param;
  LogflWindow *self = user_data;
  AdwDialog *dlg = adw_about_dialog_new ();
  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (dlg),
                                         "Log for Linux");
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (dlg),
                                logfl_engine_version ());
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (dlg),
                                       "Richard Fakenberg, OK1BR");
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (dlg),
                                     GTK_LICENSE_GPL_3_0);
  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (dlg),
                                "https://github.com/OK1BR/log-for-linux");
  char *dbg = g_strdup_printf (
      "SQLite %s\nLog: %s\nTCI: %s:%u\nSettings: %s/log-for-linux/settings.ini",
      logfl_engine_sqlite_version (), self->db_path,
      self->settings.tci_host ? self->settings.tci_host
                              : LOGFL_TCI_DEFAULT_HOST,
      self->settings.tci_port ? self->settings.tci_port
                              : LOGFL_TCI_DEFAULT_PORT,
      g_get_user_config_dir ());
  adw_about_dialog_set_debug_info (ADW_ABOUT_DIALOG (dlg), dbg);
  g_free (dbg);
  adw_dialog_present (dlg, GTK_WIDGET (self));
}

/* --- QSO table ---------------------------------------------------------- */

static void
col_setup (GtkSignalListItemFactory *factory, GObject *object,
           gpointer user_data)
{
  (void) factory;
  (void) user_data;
  GtkWidget *label = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_list_item_set_child (GTK_LIST_ITEM (object), label);
}

static void
col_bind (GtkSignalListItemFactory *factory, GObject *object,
          gpointer user_data)
{
  (void) user_data;
  int col = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (factory), "col"));
  GtkListItem *item = GTK_LIST_ITEM (object);
  GtkLabel *label = GTK_LABEL (gtk_list_item_get_child (item));
  const LogflQso *q = logfl_qso_row_qso (gtk_list_item_get_item (item));
  char *tmp = NULL;

  switch (col)
    {
    case COL_UTC:
      {
        GDateTime *dt = g_date_time_new_from_unix_utc (q->ts);
        tmp = g_date_time_format (dt, "%d.%m.%y %H:%M");
        g_date_time_unref (dt);
        gtk_label_set_text (label, tmp);
        break;
      }
    case COL_CALL:
      gtk_label_set_text (label, q->call);
      break;
    case COL_BAND:
      gtk_label_set_text (label, q->band);
      break;
    case COL_FREQ:
      tmp = fmt_freq (q->freq);
      gtk_label_set_text (label, tmp);
      break;
    case COL_MODE:
      if (q->submode && *q->submode)
        tmp = g_strdup_printf ("%s/%s", q->mode, q->submode);
      gtk_label_set_text (label, tmp ? tmp : q->mode);
      break;
    case COL_RST:
      tmp = g_strdup_printf ("%s/%s",
                             q->rst_sent ? q->rst_sent : "—",
                             q->rst_rcvd ? q->rst_rcvd : "—");
      gtk_label_set_text (label, tmp);
      break;
    case COL_NAME:
      gtk_label_set_text (label, q->name ? q->name : "");
      break;
    case COL_COMMENT:
      gtk_label_set_text (label, q->comment ? q->comment : "");
      break;
    }
  g_free (tmp);
}

static void
add_column (GtkColumnView *view, const char *title, int col,
            gboolean expand)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_object_set_data (G_OBJECT (factory), "col", GINT_TO_POINTER (col));
  g_signal_connect (factory, "setup", G_CALLBACK (col_setup), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (col_bind), NULL);
  GtkColumnViewColumn *c = gtk_column_view_column_new (title, factory);
  gtk_column_view_column_set_resizable (c, TRUE);
  gtk_column_view_column_set_expand (c, expand);
  gtk_column_view_append_column (view, c);
  g_object_unref (c);
}

/* --- construction ------------------------------------------------------- */

static GtkWidget *
labeled (const char *caption, GtkWidget *child)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *label = gtk_label_new (caption);
  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_widget_add_css_class (label, "caption");
  gtk_widget_add_css_class (label, "dim-label");
  gtk_box_append (GTK_BOX (box), label);
  gtk_box_append (GTK_BOX (box), child);
  return box;
}

static GtkWidget *
mk_entry (LogflWindow *self, int width_chars, const char *placeholder)
{
  GtkWidget *e = gtk_entry_new ();
  gtk_editable_set_width_chars (GTK_EDITABLE (e), width_chars);
  if (placeholder)
    gtk_entry_set_placeholder_text (GTK_ENTRY (e), placeholder);
  /* Enter: ESM cycle when enabled, otherwise log / save edit. */
  g_signal_connect_swapped (e, "activate", G_CALLBACK (on_entry_activate),
                            self);
  return e;
}

static gboolean
show_store_open_error (gpointer user_data)
{
  LogflWindow *self = user_data;
  if (!self->store_open_error)
    return G_SOURCE_REMOVE;

  AdwDialog *dlg = adw_alert_dialog_new ("Cannot open log", NULL);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (dlg),
      "The log database could not be opened:\n\n%s\n\nPath: %s",
      self->store_open_error,
      self->db_path ? self->db_path : "(unknown)");
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "close", "Close", NULL);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "close");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "close");
  adw_dialog_present (dlg, GTK_WIDGET (self));
  g_clear_pointer (&self->store_open_error, g_free);
  return G_SOURCE_REMOVE;
}

static void
show_log_window (LogflWindow *self)
{
  if (!self->log_win)
    return;
  gtk_window_present (GTK_WINDOW (self->log_win));
}

static void
act_show_log (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void) action;
  (void) param;
  show_log_window (user_data);
}

static void
logfl_window_dispose (GObject *obj)
{
  LogflWindow *self = LOGFL_WINDOW (obj);
  g_clear_handle_id (&self->clock_id, g_source_remove);
  g_clear_handle_id (&self->search_id, g_source_remove);
  g_clear_handle_id (&self->tci_retry_id, g_source_remove);
  /* Sentinel for in-flight TCI idles / connect jobs: null the status label
   * first so they drop work instead of touching a half-torn window. */
  self->tci_label = NULL;
  if (self->tci)
    {
      logfl_tci_client_set_state_cb (self->tci, NULL, NULL);
      logfl_tci_client_set_closed_cb (self->tci, NULL, NULL);
      logfl_tci_client_free (self->tci);
      self->tci = NULL;
    }
  g_clear_pointer (&self->pending, logfl_qso_free);
  g_clear_pointer (&self->editing, logfl_qso_free);
  g_clear_pointer (&self->store_open_error, g_free);
  /* Destroy the QSO list window before dropping the list model it uses. */
  if (self->log_win)
    {
      gtk_window_destroy (GTK_WINDOW (self->log_win));
      self->log_win = NULL;
      self->log_title = NULL;
      self->search = NULL;
      self->delete_btn = NULL;
      self->edit_btn = NULL;
    }
  /* Drop our refs; the column view may still hold one on selection until
   * the widget tree is torn down. */
  g_clear_object (&self->selection);
  g_clear_object (&self->rows);
  g_clear_pointer (&self->store, logfl_store_close);
  g_clear_pointer (&self->db_path, g_free);
  logfl_settings_clear (&self->settings);
  G_OBJECT_CLASS (logfl_window_parent_class)->dispose (obj);
}

static void
logfl_window_class_init (LogflWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = logfl_window_dispose;
}

static const GActionEntry win_actions[] = {
  { .name = "import", .activate = act_import },
  { .name = "export", .activate = act_export },
  { .name = "preferences", .activate = act_preferences },
  { .name = "show-log", .activate = act_show_log },
  { .name = "about", .activate = act_about },
};

/* Secondary window: QSO table + search + delete. Hide-on-close; reopened
 * from the main header list icon (before the hamburger). */
static void
build_log_window (LogflWindow *self)
{
  self->rows = g_list_store_new (LOGFL_TYPE_QSO_ROW);
  self->selection =
      gtk_single_selection_new (G_LIST_MODEL (g_object_ref (self->rows)));
  gtk_single_selection_set_autoselect (self->selection, FALSE);

  GtkWidget *view = gtk_column_view_new (GTK_SELECTION_MODEL (self->selection));
  gtk_widget_add_css_class (view, "data-table");
  g_signal_connect (view, "activate", G_CALLBACK (on_row_activate), self);
  add_column (GTK_COLUMN_VIEW (view), "UTC", COL_UTC, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "Call", COL_CALL, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "Band", COL_BAND, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "MHz", COL_FREQ, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "Mode", COL_MODE, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "RST", COL_RST, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "Name", COL_NAME, FALSE);
  add_column (GTK_COLUMN_VIEW (view), "Comment", COL_COMMENT, TRUE);

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_set_hexpand (scroller, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), view);

  GtkWidget *header = adw_header_bar_new ();
  self->log_title =
      ADW_WINDOW_TITLE (adw_window_title_new ("QSO log", NULL));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   GTK_WIDGET (self->log_title));

  self->search = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (self->search),
                                         "Search call, name, QTH…");
  gtk_widget_set_size_request (self->search, 240, -1);
  g_signal_connect_swapped (self->search, "search-changed",
                            G_CALLBACK (on_search_changed), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), self->search);

  self->delete_btn = gtk_button_new_from_icon_name ("user-trash-symbolic");
  gtk_widget_set_tooltip_text (self->delete_btn, "Delete selected QSO");
  g_signal_connect (self->delete_btn, "clicked",
                    G_CALLBACK (on_delete_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), self->delete_btn);

  self->edit_btn =
      gtk_button_new_from_icon_name ("document-edit-symbolic");
  gtk_widget_set_tooltip_text (self->edit_btn,
                               "Edit selected QSO in the entry row "
                               "(double-click still QSYs)");
  g_signal_connect (self->edit_btn, "clicked",
                    G_CALLBACK (on_edit_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), self->edit_btn);

  GtkWidget *tbv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tbv), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tbv), scroller);

  /* Transient to the main window so it stays associated; not modal. */
  self->log_win = gtk_window_new ();
  gtk_window_set_title (GTK_WINDOW (self->log_win), "QSO log");
  gtk_window_set_default_size (GTK_WINDOW (self->log_win), 1100, 600);
  gtk_window_set_hide_on_close (GTK_WINDOW (self->log_win), TRUE);
  gtk_window_set_transient_for (GTK_WINDOW (self->log_win),
                                GTK_WINDOW (self));
  gtk_window_set_destroy_with_parent (GTK_WINDOW (self->log_win), TRUE);
  gtk_window_set_child (GTK_WINDOW (self->log_win), tbv);
  /* Application association keeps the process alive while only this window
   * is visible; main is the primary window. */
  {
    GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
    if (app)
      gtk_application_add_window (app, GTK_WINDOW (self->log_win));
  }
}

static void
logfl_window_init (LogflWindow *self)
{
  gtk_window_set_title (GTK_WINDOW (self), "Log for Linux");
  /* Entry-focused main window — the table lives in a separate window. */
  gtk_window_set_default_size (GTK_WINDOW (self), 1100, 300);
  g_action_map_add_action_entries (G_ACTION_MAP (self), win_actions,
                                   G_N_ELEMENTS (win_actions), self);

  logfl_settings_load (&self->settings);

  /* Store. */
  GError *err = NULL;
  char *dir = g_build_filename (g_get_user_data_dir (), "log-for-linux",
                                NULL);
  g_mkdir_with_parents (dir, 0700);
  self->db_path = g_build_filename (dir, "log.db", NULL);
  g_free (dir);
  self->store = logfl_store_open (self->db_path, &err);
  if (!self->store)
    {
      /* Do not abort the process (g_error); surface a dialog once the
       * window is up and keep the UI empty until the user closes it. */
      self->store_open_error = g_strdup (err ? err->message : "unknown error");
      g_clear_error (&err);
      g_idle_add (show_store_open_error, self);
    }

  /* Header bar: title; end = log icon · Run/S&P · hamburger (pack_end is
   * right-to-left, so menu first). */
  GtkWidget *header = adw_header_bar_new ();
  self->title = ADW_WINDOW_TITLE (adw_window_title_new ("Log for Linux",
                                                        NULL));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   GTK_WIDGET (self->title));

  GMenu *menu = g_menu_new ();
  g_menu_append (menu, "_Import ADIF…", "win.import");
  g_menu_append (menu, "_Export ADIF…", "win.export");
  g_menu_append (menu, "_QSO log", "win.show-log");
  g_menu_append (menu, "_Preferences", "win.preferences");
  g_menu_append (menu, "_About Log for Linux", "win.about");
  GtkWidget *menu_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn),
                                 "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_btn),
                                  G_MENU_MODEL (menu));
  g_object_unref (menu);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_btn);

  /* Between list icon and menu — compact linked Run / S&P for macros. */
  adw_header_bar_pack_end (ADW_HEADER_BAR (header),
                           build_bank_header_toggle (self));

  GtkWidget *log_open_btn =
      gtk_button_new_from_icon_name ("view-list-symbolic");
  gtk_widget_set_tooltip_text (log_open_btn, "Open QSO log");
  gtk_actionable_set_action_name (GTK_ACTIONABLE (log_open_btn),
                                  "win.show-log");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), log_open_btn);

  /* Entry row. */
  self->call = mk_entry (self, 10, "OK1…");
  g_signal_connect_swapped (self->call, "changed",
                            G_CALLBACK (update_wb4), self);
  self->rst_s = mk_entry (self, 4, NULL);
  self->rst_r = mk_entry (self, 4, NULL);
  gtk_editable_set_text (GTK_EDITABLE (self->rst_s), "599");
  gtk_editable_set_text (GTK_EDITABLE (self->rst_r), "599");
  self->band_dd = gtk_drop_down_new_from_strings (bands);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (self->band_dd), 5); /* 40m */
  g_signal_connect_swapped (self->band_dd, "notify::selected",
                            G_CALLBACK (on_band_changed), self);
  self->mode_dd = gtk_drop_down_new_from_strings (modes);
  g_signal_connect_swapped (self->mode_dd, "notify::selected",
                            G_CALLBACK (on_mode_changed), self);
  self->freq = mk_entry (self, 8, "7.030");
  g_signal_connect_swapped (self->freq, "changed",
                            G_CALLBACK (on_freq_changed), self);
  /* Seed mid-band so a quick Log QSO still writes freq (TCI overwrites). */
  {
    double mhz = logfl_adif_freq_for_band ("40m");
    if (mhz > 0)
      {
        char *txt = fmt_freq (mhz);
        gtk_editable_set_text (GTK_EDITABLE (self->freq), txt);
        g_free (txt);
      }
  }
  self->name = mk_entry (self, 12, NULL);
  self->comment = mk_entry (self, 18, NULL);
  gtk_widget_set_hexpand (self->comment, TRUE);

  self->log_btn = gtk_button_new_with_label ("Log QSO");
  gtk_widget_add_css_class (self->log_btn, "suggested-action");
  gtk_widget_set_valign (self->log_btn, GTK_ALIGN_END);
  g_signal_connect_swapped (self->log_btn, "clicked",
                            G_CALLBACK (log_qso), self);

  self->cancel_edit_btn = gtk_button_new_with_label ("Cancel");
  gtk_widget_set_valign (self->cancel_edit_btn, GTK_ALIGN_END);
  gtk_widget_set_visible (self->cancel_edit_btn, FALSE);
  g_signal_connect_swapped (self->cancel_edit_btn, "clicked",
                            G_CALLBACK (cancel_edit_qso), self);

  GtkWidget *fields = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (fields), labeled ("Call", self->call));
  gtk_box_append (GTK_BOX (fields), labeled ("RST s", self->rst_s));
  gtk_box_append (GTK_BOX (fields), labeled ("RST r", self->rst_r));
  gtk_box_append (GTK_BOX (fields), labeled ("Band", self->band_dd));
  gtk_box_append (GTK_BOX (fields), labeled ("Mode", self->mode_dd));
  gtk_box_append (GTK_BOX (fields), labeled ("MHz", self->freq));
  gtk_box_append (GTK_BOX (fields), labeled ("Name", self->name));
  gtk_box_append (GTK_BOX (fields), labeled ("Comment", self->comment));
  gtk_box_append (GTK_BOX (fields), self->cancel_edit_btn);
  gtk_box_append (GTK_BOX (fields), self->log_btn);

  /* Worked-B4 stays with the entry row (depends on call/band/mode). */
  self->wb4_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->wb4_label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (self->wb4_label),
                           PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (self->wb4_label, TRUE);
  gtk_widget_add_css_class (self->wb4_label, "dim-label");

  GtkWidget *macro_bar = build_macro_bar (self);

  GtkWidget *entry_bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (entry_bar, 10);
  gtk_widget_set_margin_bottom (entry_bar, 10);
  gtk_widget_set_margin_start (entry_bar, 12);
  gtk_widget_set_margin_end (entry_bar, 12);
  gtk_box_append (GTK_BOX (entry_bar), fields);
  gtk_box_append (GTK_BOX (entry_bar), macro_bar);
  gtk_box_append (GTK_BOX (entry_bar), self->wb4_label);

  /* F1–F8 / Esc (STOP) from the entry window (contest-logger style). */
  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_main_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);

  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toasts, entry_bar);

  /* Footer: UTC clock/date (left) · active TCI status (right). */
  self->clock_label = gtk_label_new ("");
  gtk_widget_add_css_class (self->clock_label, "numeric");
  gtk_widget_add_css_class (self->clock_label, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (self->clock_label), 0);
  self->tci_label = gtk_label_new ("TCI offline");
  gtk_widget_add_css_class (self->tci_label, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (self->tci_label), 1);
  gtk_label_set_ellipsize (GTK_LABEL (self->tci_label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (self->tci_label, TRUE);

  GtkWidget *footer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_set_margin_top (footer, 4);
  gtk_widget_set_margin_bottom (footer, 4);
  gtk_widget_set_margin_start (footer, 12);
  gtk_widget_set_margin_end (footer, 12);
  gtk_box_append (GTK_BOX (footer), self->clock_label);
  gtk_box_append (GTK_BOX (footer), self->tci_label);

  GtkWidget *tbv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tbv), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tbv),
                                GTK_WIDGET (self->toasts));
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (tbv), footer);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), tbv);

  build_log_window (self);

  self->clock_id = g_timeout_add_seconds (1, clock_tick, self);
  clock_tick (self);
  reload (self);
  /* M4: connect to sdr-for-linux TCI in a background thread (non-blocking). */
  g_idle_add (tci_connect_kick, self);
  gtk_widget_grab_focus (self->call);
}

GtkWidget *
logfl_window_new (AdwApplication *app)
{
  return g_object_new (LOGFL_TYPE_WINDOW, "application", app, NULL);
}
