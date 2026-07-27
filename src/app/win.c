/* win.c — main logbook window: entry row, macro strip, QSO table (search /
 * inline cell edit / right-click delete), footer with UTC, TCI and WSJT-X
 * status. ADIF import/export and preferences live on the window menu
 * (M3/M4/M6).
 *
 * Table cells: plain label that a single click swaps for a GtkEntry (the
 * row hover highlight marks the edit target; no selection model). A
 * right-click on a row opens the delete confirm dialog directly. Entry
 * strip is for new QSOs only. TCI: VFO/mode prefill + CW macros.
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "win.h"

#include <string.h>

#include "adif.h"
#include "contest.h"
#include "engine.h"
#include "log_store.h"
#include "macros.h"
#include "qso_row.h"
#include "settings.h"
#include "tci_client.h"
#include "wsjtx_udp.h"

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

enum { COL_UTC, COL_CALL, COL_BAND, COL_FREQ, COL_MODE, COL_RST_S,
       COL_RST_R, COL_NAME, COL_COMMENT, COL_STX, COL_EXCH };

struct _LogflWindow {
  AdwApplicationWindow parent_instance;

  LogflStore *store;
  char *db_path;
  char *store_open_error;      /* non-NULL when open failed; shown once */
  LogflSettings settings;      /* ~/.config/log-for-linux/settings.ini   */

  GListStore *rows;
  GtkSelectionModel *selection; /* GtkNoSelection — no click highlight */

  AdwWindowTitle *title;
  AdwToastOverlay *toasts;
  GtkWidget *search;
  GtkWidget *call, *rst_s, *rst_r, *freq, *name, *comment;
  GtkWidget *band_dd, *mode_dd;
  GtkWidget *wb4_label, *clock_label, *tci_label, *wsjtx_label;
  GtkWidget *table_view;       /* GtkColumnView — right-click context menu */
  GtkWidget *cell_edit_box;    /* non-NULL while a table cell entry is open */
  GtkWidget *log_btn;          /* "Log QSO" */
  GtkWidget *macro_btns[LOGFL_MACRO_N_KEYS];
  GtkWidget *bank_btn;         /* header: single Run/S&P icon (cycles) */
  GtkWidget *esm_hint;         /* short ESM status under macro bar */

  guint clock_id;
  guint search_id;             /* debounce timeout for search-changed */
  guint tci_retry_id;          /* reconnect timer when TCI is down */
  gboolean syncing_freq;       /* guard against freq↔band feedback */
  gboolean syncing_tci;        /* guard: applying radio state to entry */
  gboolean tci_connecting;     /* connect thread in flight */
  guint tci_epoch;             /* bumps on reconnect; stale jobs drop */
  LogflTciClient *tci;
  LogflWsjtxServer *wsjtx;     /* M6: UDP listener for WSJT-X / JTDX        */
  LogflQso *pending;           /* QSO awaiting dup confirmation */
  gint64 pending_delete_id;
  gint64 context_qso_id;       /* row under last right-click (delete menu) */
  gboolean delete_confirm_open; /* async delete dialog is up */
  LogflEsmPhase esm_phase;     /* M5 Enter-sends-message state */
  gboolean esm_force_log;      /* ESM LOG step → bypass ESM on Enter */
  gboolean prefs_macros_dirty; /* macro edits in Preferences await ini save */

  /* M9 contests. */
  LogflContest *contest;       /* active contest; NULL = main log view */
  LogflExchDef *exch_def;      /* parsed template of the active contest */
  GtkWidget *exch_box;         /* entry-row slot holding template fields */
  GPtrArray *exch_entries;     /* GtkEntry* per template field (unowned) */
  GtkWidget *serial_value;     /* "Sent" entry, prefilled with the next
                                * serial (or static exchange) — editable */
  GtkWidget *contest_btn;      /* header switcher (menu button) */
  GtkColumnViewColumn *col_stx, *col_exch; /* contest-only table columns */
  GtkColumnViewColumn *col_name, *col_comment; /* hidden while in contest */
  guint next_serial;           /* next sent serial (0 = no serial contest) */
  gint64 pending_contest_delete; /* contest id in the open delete confirm */
};

G_DEFINE_FINAL_TYPE (LogflWindow, logfl_window, ADW_TYPE_APPLICATION_WINDOW)

static void refresh_esm_hint (LogflWindow *self);
static GtkWidget *labeled (const char *caption, GtkWidget *child);
static void contest_ui_refresh (LogflWindow *self);
static void refresh_serial (LogflWindow *self);
static GtkWidget *mk_entry (LogflWindow *self, int width_chars,
                            const char *placeholder);

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

/* TX confirmations fire on every macro key — keep them short so a Run
 * session does not stack toasts over the table. */
static void
toast_short (LogflWindow *self, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  char *msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  AdwToast *t = adw_toast_new (msg);
  adw_toast_set_timeout (t, 2);
  adw_toast_overlay_add_toast (self->toasts, t);
  g_free (msg);
}

static char *
fmt_freq (double mhz)
{
  if (mhz <= 0)
    return g_strdup ("");
  /* Hz-exact: always six decimals, zero-padded — never shortened. */
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd (buf, sizeof buf, "%.6f", mhz);
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

/* Subtitle mirrors the active view: contest name + its counters when
 * switched into a contest, whole-log counters otherwise. */
static void
update_subtitle (LogflWindow *self)
{
  if (!self->store)
    return;
  LogflStoreStats st;
  char *sub = NULL;
  if (self->contest)
    {
      if (logfl_store_contest_stats (self->store, self->contest->id, &st,
                                     NULL))
        sub = g_strdup_printf ("%s — %u QSO · %u calls",
                               self->contest->name, st.n_qso, st.n_calls);
    }
  else if (logfl_store_stats (self->store, &st, NULL))
    sub = g_strdup_printf ("%u QSO · %u calls", st.n_qso, st.n_calls);
  if (sub)
    {
      adw_window_title_set_subtitle (self->title, sub);
      g_free (sub);
    }
}

static void
reload (LogflWindow *self)
{
  if (!self->store)
    {
      if (self->rows)
        g_list_store_remove_all (self->rows);
      adw_window_title_set_subtitle (self->title, "log store unavailable");
      return;
    }

  GError *err = NULL;
  const char *text = (self->search && entry_text (self->search)[0])
                         ? entry_text (self->search)
                         : NULL;
  /* Main view = QSOs outside any contest; a contest view = its QSOs only. */
  LogflStoreQuery q = { .text = text,
                        .contest = self->contest ? self->contest->id
                                                 : LOGFL_QUERY_CONTEST_NONE };
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

  update_subtitle (self);
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

typedef struct {
  LogflWindow  *self;          /* strong ref — idle may outlive the window */
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
      g_object_unref (self);
      g_free (d);
      return G_SOURCE_REMOVE;
    }

  /* Prefill entry from live VFO (entry strip is for new QSOs only). */
  self->syncing_tci = TRUE;
  if (st->vfo_hz > 0)
    {
      char *mhz = fmt_freq (st->vfo_hz / 1e6);
      gtk_editable_set_text (GTK_EDITABLE (self->freq), mhz);
      g_free (mhz);
    }
  {
    const char *log_mode = logfl_tci_mode_to_log (st->mode);
    if (log_mode)
      select_mode_string (self, log_mode);
  }
  self->syncing_tci = FALSE;
  update_wb4 (self);

  char *mhz_txt = st->vfo_hz > 0 ? fmt_freq (st->vfo_hz / 1e6) : g_strdup ("—");
  char *status = g_strdup_printf (
      "TCI · %s · %s MHz · %s",
      st->device[0] ? st->device : "radio",
      mhz_txt,
      st->mode[0] ? st->mode : "—");
  tci_set_status (self, status);
  g_free (status);
  g_free (mhz_txt);

  g_object_unref (self);
  g_free (d);
  return G_SOURCE_REMOVE;
}

/* LWS thread → main loop. user_data is the window (stable while client lives). */
static void
on_tci_state (const LogflTciState *st, gpointer user_data)
{
  LogflWindow *self = user_data;
  TciStateIdle *d = g_new (TciStateIdle, 1);
  d->self = g_object_ref (self);
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
  LogflWindow *self = user_data;   /* strong ref from on_tci_closed */
  if (self->tci_label == NULL)
    {
      g_object_unref (self);
      return G_SOURCE_REMOVE;
    }

  if (self->tci)
    {
      logfl_tci_client_set_state_cb (self->tci, NULL, NULL);
      logfl_tci_client_set_closed_cb (self->tci, NULL, NULL);
      logfl_tci_client_free (self->tci);
      self->tci = NULL;
    }
  tci_set_status (self, "TCI offline");
  tci_schedule_connect (self);
  g_object_unref (self);
  return G_SOURCE_REMOVE;
}

static void
on_tci_closed (gpointer user_data)
{
  g_idle_add (tci_mark_offline, g_object_ref (user_data));
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
  /* No "connecting…" status here — the 5 s background retry against a dead
   * server fails in milliseconds and the footer would just blink. The label
   * changes only on real transitions (ready / offline / manual reconnect). */
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

/* --- entry row logic ---------------------------------------------------- */

static void
update_wb4 (LogflWindow *self)
{
  const char *call = entry_text (self->call);
  GtkWidget *l = self->wb4_label;

  gtk_widget_remove_css_class (l, "success");
  gtk_widget_remove_css_class (l, "warning");
  gtk_widget_remove_css_class (l, "error");
  if (!self->store || strlen (call) < 2)
    {
      gtk_label_set_text (GTK_LABEL (l), "");
      return;
    }

  /* In a contest the question is "is this a dup HERE" — same call+band+mode
   * already in this contest, any time. Worked-B4 stays the global answer. */
  gboolean contest_dup = FALSE;
  if (self->contest)
    logfl_store_contest_dup_check (self->store, self->contest->id, call,
                                   dd_selected (self->band_dd, bands),
                                   dd_selected (self->mode_dd, modes),
                                   &contest_dup, NULL);

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
      "%sB4: %u× · this band %u× · band+mode %u× · last %s",
      contest_dup ? "DUP in contest · " : "",
      wb.n_total, wb.n_band, wb.n_band_mode, when);
  gtk_label_set_text (GTK_LABEL (l), txt);
  gtk_widget_add_css_class (l, contest_dup ? "error" : "warning");
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

/* Operator picked a band: keep the MHz field consistent with it. A typed
 * (or TCI) frequency already inside the band is left alone; anything else —
 * empty field or a leftover from the previous band — is reseeded with the
 * band mid-point so the logged QSO never carries band/freq that disagree. */
static void
on_band_changed (LogflWindow *self)
{
  if (self->syncing_freq || self->syncing_tci)
    {
      update_wb4 (self);
      return;
    }
  const char *band = dd_selected (self->band_dd, bands);
  char *cur = g_strdup (entry_text (self->freq));
  g_strdelimit (cur, ",", '.');
  double cur_mhz = g_ascii_strtod (cur, NULL);
  g_free (cur);
  const char *cur_band =
      cur_mhz > 0 ? logfl_adif_band_for_freq (cur_mhz) : NULL;
  if (band && (!cur_band || !g_str_equal (cur_band, band)))
    {
      double mhz = logfl_adif_freq_for_band (band);
      if (mhz > 0)
        {
          char *txt = fmt_freq (mhz);
          self->syncing_freq = TRUE;
          gtk_editable_set_text (GTK_EDITABLE (self->freq), txt);
          self->syncing_freq = FALSE;
          g_free (txt);
        }
    }
  update_wb4 (self);
}

static void
clear_entry_row (LogflWindow *self)
{
  gtk_editable_set_text (GTK_EDITABLE (self->call), "");
  gtk_editable_set_text (GTK_EDITABLE (self->name), "");
  gtk_editable_set_text (GTK_EDITABLE (self->comment), "");
  for (guint i = 0; self->exch_entries && i < self->exch_entries->len; i++)
    gtk_editable_set_text (GTK_EDITABLE (self->exch_entries->pdata[i]), "");
  gtk_label_set_text (GTK_LABEL (self->wb4_label), "");
  gtk_widget_grab_focus (self->call);
}

/* Resolve MHz for a new log: typed entry → live TCI VFO → band mid-point. */
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

/* Apply entry-row fields onto a new QSO (timestamp, station, freq resolve). */
static void
apply_entry_to_qso (LogflWindow *self, LogflQso *q)
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

  q->ts = g_get_real_time () / G_USEC_PER_SEC;
  q->freq = resolve_log_freq (self, q->band);
  g_free (q->station_callsign);
  q->station_callsign = g_strdup (
      self->settings.station_callsign && *self->settings.station_callsign
          ? self->settings.station_callsign
          : "OK1BR");

  /* Active contest: link the QSO and route the exchange per template.
   * The Sent entry wins over the auto values — the operator may have sent
   * a different serial (or exchange) than the prefill. */
  if (self->contest && self->exch_def)
    {
      q->contest_ref = self->contest->id;
      guint n = self->exch_entries->len;
      const char **vals = g_new0 (const char *, n + 1);
      for (guint i = 0; i < n; i++)
        vals[i] = entry_text (self->exch_entries->pdata[i]);

      char *sent = self->serial_value
                       ? g_strstrip (g_strdup (entry_text (self->serial_value)))
                       : g_strdup ("");
      if (self->exch_def->tx_serial)
        {
          guint serial = self->next_serial;
          if (*sent)
            {
              gboolean digits = TRUE;
              for (const char *c = sent; *c; c++)
                if (!g_ascii_isdigit (*c))
                  digits = FALSE;
              if (digits)
                serial = (guint) g_ascii_strtoull (sent, NULL, 10);
            }
          logfl_exch_apply (self->exch_def, vals, n, self->contest->my_exch,
                            serial, q);
        }
      else
        logfl_exch_apply (self->exch_def, vals, n,
                          *sent ? sent : self->contest->my_exch, 0, q);
      g_free (sent);
      g_free (vals);
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
      refresh_serial (self);
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
  apply_entry_to_qso (self, q);

  /* Contest rules: one QSO per call+band+mode for the whole contest;
   * outside a contest the ±5 min window applies. */
  gboolean dup = FALSE;
  if (self->contest)
    logfl_store_contest_dup_check (self->store, self->contest->id, q->call,
                                   q->band, q->mode, &dup, NULL);
  else
    logfl_store_dup_check (self->store, q->call, q->band, q->mode, q->ts,
                           DUP_WINDOW_S, &dup, NULL);
  self->pending = q;
  if (!dup)
    {
      do_add_pending (self);
      return;
    }

  AdwDialog *dlg = adw_alert_dialog_new ("Duplicate?", NULL);
  if (self->contest)
    adw_alert_dialog_format_body (ADW_ALERT_DIALOG (dlg),
        "%s was already worked in %s on %s/%s.",
        q->call, self->contest->name, q->band, q->mode);
  else
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
          toast_short (self, "CW stop");
        }
      else
        toast_short (self, "TCI not connected");
      return;
    }

  const LogflMacroKey *k = active_macro_key (self, idx);
  if (!k)
    return;

  const char *mycall =
      self->settings.station_callsign && *self->settings.station_callsign
          ? self->settings.station_callsign
          : "OK1BR";
  /* {NR}/{EXCH} follow the Sent entry — what the operator sees is what
   * the keyer sends. */
  const char *sent_txt = self->contest && self->serial_value
                             ? entry_text (self->serial_value)
                             : NULL;
  const char *exch = self->contest ? self->contest->my_exch : NULL;
  char *nr = NULL;
  if (self->contest && self->exch_def && self->exch_def->tx_serial)
    nr = sent_txt && *sent_txt
             ? g_strdup (sent_txt)
             : (self->next_serial
                    ? logfl_exch_serial_format (self->next_serial)
                    : NULL);
  else if (sent_txt && *sent_txt)
    exch = sent_txt;
  char *msg = logfl_macro_expand (k->tmpl, mycall, entry_text (self->call),
                                  entry_text (self->rst_s), nr, exch);
  g_free (nr);
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
  toast_short (self, "TX %s: %s", cap, msg);
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
        "Tokens: {MYCALL} {CALL} {RST} {NR} {EXCH}"
        "  ·  empty text = unused free slot");

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
refresh_bank_btn (LogflWindow *self)
{
  if (!self->bank_btn)
    return;
  gboolean snp = self->settings.macro_bank == LOGFL_MACRO_BANK_SNP;
  /* Run = continuous CQ; S&P = search for stations. */
  gtk_button_set_icon_name (GTK_BUTTON (self->bank_btn),
                            snp ? "edit-find-symbolic"
                                : "media-playlist-consecutive-symbolic");
  gtk_widget_set_tooltip_text (
      self->bank_btn,
      snp ? "S&P message bank — click for Run"
          : "Run message bank — click for S&P");
}

static void
set_macro_bank (LogflWindow *self, LogflMacroBankId bank)
{
  if (self->settings.macro_bank == bank)
    {
      refresh_bank_btn (self);
      return;
    }
  self->settings.macro_bank = bank;
  logfl_settings_save (&self->settings);
  refresh_macro_bar (self);
  refresh_bank_btn (self);
}

static void
on_bank_btn_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  LogflWindow *self = user_data;
  LogflMacroBankId next =
      self->settings.macro_bank == LOGFL_MACRO_BANK_RUN
          ? LOGFL_MACRO_BANK_SNP
          : LOGFL_MACRO_BANK_RUN;
  set_macro_bank (self, next);
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

/* Enter in entry fields: ESM cycle when enabled, else log. */
static void
on_entry_activate (LogflWindow *self)
{
  if (self->esm_force_log || !self->settings.esm_enabled)
    {
      log_qso (self);
      return;
    }
  esm_enter (self);
}

/* Forward: cancel inline cell edit (window Esc runs in CAPTURE before the
 * entry controller, so Esc must be handled here while a cell is open). */
static void cell_end_edit (GtkWidget *box, gboolean commit);

static gboolean
on_main_key (GtkEventControllerKey *ctl, guint keyval, guint keycode,
             GdkModifierType state, gpointer user_data)
{
  (void) ctl;
  (void) keycode;
  LogflWindow *self = user_data;
  if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
    return FALSE;
  /* A dialog is up (prefs, macro edit, confirm…): let it have the keys —
   * F1 must not key CW from a dialog entry and Esc must close the dialog. */
  if (adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (self)))
    return FALSE;
  /* Tab cycles only the hot entry fields: Call → RST s → RST r → Sent →
   * exchange (2026-07-27, Richard). Band/Mode/MHz/Name/Comment stay out
   * of the chain — mouse territory. Elsewhere Tab keeps its default. */
  if ((keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab)
      && !self->cell_edit_box)
    {
      GtkWidget *chain[16];
      guint n = 0;
      chain[n++] = self->call;
      chain[n++] = self->rst_s;
      chain[n++] = self->rst_r;
      if (self->contest && self->serial_value)
        chain[n++] = self->serial_value;
      for (guint i = 0;
           self->contest && self->exch_entries
             && i < self->exch_entries->len && n < G_N_ELEMENTS (chain);
           i++)
        chain[n++] = self->exch_entries->pdata[i];

      GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (self));
      if (focus)
        for (guint i = 0; i < n; i++)
          if (focus == chain[i] || gtk_widget_is_ancestor (focus, chain[i]))
            {
              gboolean back = keyval == GDK_KEY_ISO_Left_Tab
                              || (state & GDK_SHIFT_MASK) != 0;
              gtk_widget_grab_focus (chain[back ? (i + n - 1) % n
                                                : (i + 1) % n]);
              return TRUE;
            }
    }
  if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F8)
    {
      /* Do not fire macros while typing in a cell. */
      if (self->cell_edit_box)
        return FALSE;
      macro_run (self, keyval - GDK_KEY_F1);
      return TRUE;
    }
  if (keyval == GDK_KEY_Escape)
    {
      /* Prefer discard of inline edit over CW stop (second Esc still stops). */
      if (self->cell_edit_box)
        {
          cell_end_edit (self->cell_edit_box, FALSE);
          return TRUE;
        }
      /* Esc in the search field clears the filter (then jumps back to the
       * entry row) instead of surprising the operator with a CW stop. */
      GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (self));
      if (focus && self->search &&
          (focus == self->search ||
           gtk_widget_is_ancestor (focus, self->search)))
        {
          if (entry_text (self->search)[0])
            gtk_editable_set_text (GTK_EDITABLE (self->search), "");
          else
            gtk_widget_grab_focus (self->call);
          return TRUE;
        }
      macro_run (self, LOGFL_MACRO_STOP_IDX);
      return TRUE;
    }
  return FALSE;
}

/* Any press (any button) outside the open cell editor discards that edit —
 * saving is Enter only. Runs in CAPTURE on the window, before the press
 * reaches its target; the event is not claimed, so the click still does
 * whatever it was aimed at (open another cell, push a button, …). */
static void
on_window_press (GtkGestureClick *gesture, gint n_press, gdouble x,
                 gdouble y, gpointer user_data)
{
  (void) gesture;
  (void) n_press;
  LogflWindow *self = user_data;
  GtkWidget *box = self->cell_edit_box;
  if (!box)
    return;
  GtkWidget *target =
      gtk_widget_pick (GTK_WIDGET (self), x, y, GTK_PICK_DEFAULT);
  if (target != NULL &&
      (target == box || gtk_widget_is_ancestor (target, box)))
    return;
  cell_end_edit (box, FALSE);
}

/* Single header icon: Run ↔ S&P, icon changes with the active bank. */
static GtkWidget *
build_bank_header_btn (LogflWindow *self)
{
  self->bank_btn = gtk_button_new_from_icon_name (
      "media-playlist-consecutive-symbolic");
  gtk_widget_set_valign (self->bank_btn, GTK_ALIGN_CENTER);
  g_signal_connect (self->bank_btn, "clicked",
                    G_CALLBACK (on_bank_btn_clicked), self);
  refresh_bank_btn (self);
  return self->bank_btn;
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

/* --- delete (confirm dialog; triggered from row context menu) ----------- */

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
      reload (self);
      /* Deleting the highest contest serial hands its number out again. */
      refresh_serial (self);
    }
  else
    {
      toast (self, "Delete failed: %s", err->message);
      g_clear_error (&err);
    }
  self->pending_delete_id = 0;
}

/* Confirm-delete the QSO last targeted by the row context menu. */
static void
confirm_delete_context_qso (LogflWindow *self)
{
  if (self->delete_confirm_open || !self->store || self->context_qso_id <= 0)
    return;

  GError *err = NULL;
  LogflQso *q = logfl_store_get (self->store, self->context_qso_id, &err);
  if (!q)
    {
      toast (self, "Cannot delete: %s",
             err ? err->message : "not found");
      g_clear_error (&err);
      self->context_qso_id = 0;
      return;
    }

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
  logfl_qso_free (q);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", "Cancel", "delete", "Delete",
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "delete",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg), GTK_WIDGET (self), NULL,
                           on_delete_response, self);
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

/* --- M6 WSJT-X UDP ------------------------------------------------------ */

static void
wsjtx_set_status (LogflWindow *self, const char *txt)
{
  if (self->wsjtx_label)
    gtk_label_set_text (GTK_LABEL (self->wsjtx_label), txt ? txt : "");
}

static void
on_wsjtx_logged (LogflQso *q, const LogflWsjtxQsoLogged *raw, gpointer user_data)
{
  LogflWindow *self = user_data;
  (void) raw;
  if (!self->store || !self->wsjtx_label)
    {
      logfl_qso_free (q);
      return;
    }
  /* Prefer station call from prefs when the packet left it empty. */
  if ((!q->station_callsign || !q->station_callsign[0]) &&
      self->settings.station_callsign && self->settings.station_callsign[0])
    {
      g_free (q->station_callsign);
      q->station_callsign = g_strdup (self->settings.station_callsign);
    }
  if (!q->band || !q->mode || !q->call || q->ts <= 0)
    {
      toast (self, "WSJT-X QSO incomplete (need call, band/freq, mode, time)");
      logfl_qso_free (q);
      return;
    }

  gboolean dup = FALSE;
  GError *err = NULL;
  logfl_store_dup_check (self->store, q->call, q->band, q->mode, q->ts, 0,
                         &dup, NULL);
  if (dup)
    {
      toast (self, "WSJT-X: %s already logged", q->call);
      logfl_qso_free (q);
      return;
    }
  if (logfl_store_add (self->store, q, &err))
    {
      toast (self, "WSJT-X: logged %s · %s · %s", q->call, q->band, q->mode);
      reload (self);
    }
  else
    {
      toast (self, "WSJT-X: not logged — %s",
             err ? err->message : "store error");
      g_clear_error (&err);
    }
  logfl_qso_free (q);
}

static void
on_wsjtx_status (const LogflWsjtxStatus *st, gpointer user_data)
{
  LogflWindow *self = user_data;
  if (!self->store || !self->wsjtx || !st || !st->dx_call || !st->dx_call[0])
    return;
  const char *band = NULL;
  if (st->dial_hz > 0)
    band = logfl_adif_band_for_freq ((double) st->dial_hz / 1e6);
  LogflWorkedB4 wb = { 0 };
  if (!logfl_store_worked_b4 (self->store, st->dx_call, band, st->mode, &wb,
                              NULL))
    return;
  logfl_wsjtx_server_highlight_b4 (self->wsjtx, st->hdr.id, st->dx_call,
                                   wb.n_total, NULL);
}

static void
wsjtx_stop (LogflWindow *self)
{
  if (self->wsjtx)
    {
      logfl_wsjtx_server_set_logged_cb (self->wsjtx, NULL, NULL);
      logfl_wsjtx_server_set_status_cb (self->wsjtx, NULL, NULL);
      logfl_wsjtx_server_free (self->wsjtx);
      self->wsjtx = NULL;
    }
  wsjtx_set_status (self, "WSJT-X off");
}

static void
wsjtx_start (LogflWindow *self)
{
  wsjtx_stop (self);
  if (!self->settings.wsjtx_enabled)
    {
      wsjtx_set_status (self, "WSJT-X off");
      return;
    }
  guint16 port = self->settings.wsjtx_port
                     ? self->settings.wsjtx_port
                     : LOGFL_WSJTX_DEFAULT_PORT;
  self->wsjtx = logfl_wsjtx_server_new (LOGFL_WSJTX_DEFAULT_HOST, port);
  logfl_wsjtx_server_set_logged_cb (self->wsjtx, on_wsjtx_logged, self);
  logfl_wsjtx_server_set_status_cb (self->wsjtx, on_wsjtx_status, self);
  GError *err = NULL;
  if (!logfl_wsjtx_server_start (self->wsjtx, &err))
    {
      char *msg = g_strdup_printf ("WSJT-X bind fail");
      wsjtx_set_status (self, msg);
      g_free (msg);
      toast (self, "WSJT-X UDP: %s", err ? err->message : "bind failed");
      g_clear_error (&err);
      logfl_wsjtx_server_free (self->wsjtx);
      self->wsjtx = NULL;
      return;
    }
  char *st = g_strdup_printf ("WSJT-X :%u", port);
  wsjtx_set_status (self, st);
  g_free (st);
}

static void
tci_reconnect_now (LogflWindow *self)
{
  /* Invalidate any in-flight connect job; it will free itself and re-kick. */
  self->tci_epoch++;
  tci_disconnect (self);
  tci_set_status (self, "TCI connecting…");   /* user asked — show feedback */
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
  GtkWidget *wsjtx_en_row = g_object_get_data (G_OBJECT (dlg), "wsjtx-en");
  GtkWidget *wsjtx_port_row = g_object_get_data (G_OBJECT (dlg), "wsjtx-port");

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
  gboolean wsjtx_en = wsjtx_en_row
                          ? adw_switch_row_get_active (ADW_SWITCH_ROW (wsjtx_en_row))
                          : self->settings.wsjtx_enabled;
  int wsjtx_port = wsjtx_port_row
                       ? (int) adw_spin_row_get_value (ADW_SPIN_ROW (wsjtx_port_row))
                       : self->settings.wsjtx_port;
  if (wsjtx_port < 1 || wsjtx_port > 65535)
    wsjtx_port = LOGFL_WSJTX_DEFAULT_PORT;

  gboolean tci_changed =
      g_strcmp0 (host, self->settings.tci_host) != 0 ||
      (guint16) port != self->settings.tci_port;
  gboolean call_changed =
      g_strcmp0 (call, self->settings.station_callsign) != 0;
  gboolean esm_changed = esm != self->settings.esm_enabled;
  gboolean wsjtx_changed =
      wsjtx_en != self->settings.wsjtx_enabled ||
      (guint16) wsjtx_port != self->settings.wsjtx_port;

  if (tci_changed || call_changed || esm_changed || wsjtx_changed)
    {
      g_free (self->settings.tci_host);
      self->settings.tci_host = host;
      self->settings.tci_port = (guint16) port;
      g_free (self->settings.station_callsign);
      self->settings.station_callsign = call;
      self->settings.esm_enabled = esm;
      self->settings.wsjtx_enabled = wsjtx_en;
      self->settings.wsjtx_port = (guint16) wsjtx_port;
      if (esm_changed)
        self->esm_phase = LOGFL_ESM_PHASE_READY;
      logfl_settings_save (&self->settings);
      if (tci_changed)
        tci_reconnect_now (self);
      if (wsjtx_changed)
        wsjtx_start (self);
      refresh_esm_hint (self);
    }
  else
    {
      g_free (host);
      g_free (call);
    }

  if (self->prefs_macros_dirty)
    {
      self->prefs_macros_dirty = FALSE;
      logfl_settings_save (&self->settings);
    }
}

/* --- Preferences → Messaging: inline macro bank editors ----------------- */

/* One expander per key: title "F1 · CQ", subtitle = template. Edits land in
 * the in-memory set (and on the strip) immediately; the ini is written once
 * on dialog close (prefs_closed). Right-click on the bar stays available. */

static void
pref_macro_retitle (AdwExpanderRow *xrow, const LogflMacroKey *k, guint idx)
{
  char *slot = macro_slot_name (idx);
  char *title = (k->caption && *k->caption)
                    ? g_strdup_printf ("%s · %s", slot, k->caption)
                    : g_strdup (slot);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (xrow), title);
  if (!logfl_macro_index_is_stop (idx))
    adw_expander_row_set_subtitle (
        xrow, (k->tmpl && *k->tmpl) ? k->tmpl : "unused");
  g_free (title);
  g_free (slot);
}

static void
on_pref_macro_changed (GtkEditable *e, gpointer user_data)
{
  LogflWindow *self = user_data;
  guint bank = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (e), "bank"));
  guint idx = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (e), "idx"));
  gboolean is_cap =
      GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (e), "is-cap"));
  const LogflMacroKey *k =
      logfl_macro_set_key (&self->settings.macros, bank, idx);
  if (!k)
    return;

  /* set_key frees the old strings — copy the half we keep before the call. */
  const char *txt = gtk_editable_get_text (e);
  char *cap = g_strdup (is_cap ? txt : k->caption);
  char *tmpl = g_strdup (is_cap ? k->tmpl : txt);
  logfl_macro_set_set_key (&self->settings.macros, bank, idx, cap, tmpl);
  g_free (cap);
  g_free (tmpl);

  pref_macro_retitle (g_object_get_data (G_OBJECT (e), "xrow"),
                      logfl_macro_set_key (&self->settings.macros, bank, idx),
                      idx);
  self->prefs_macros_dirty = TRUE;
  refresh_macro_bar (self);
}

static GtkWidget *
pref_macro_entry (LogflWindow *self, const char *title, const char *text,
                  guint bank, guint idx, gboolean is_cap, GtkWidget *xrow)
{
  GtkWidget *e = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (e), title);
  gtk_editable_set_text (GTK_EDITABLE (e), text ? text : "");
  g_object_set_data (G_OBJECT (e), "bank", GUINT_TO_POINTER (bank));
  g_object_set_data (G_OBJECT (e), "idx", GUINT_TO_POINTER (idx));
  g_object_set_data (G_OBJECT (e), "is-cap", GUINT_TO_POINTER (is_cap));
  g_object_set_data (G_OBJECT (e), "xrow", xrow);
  g_signal_connect (e, "changed", G_CALLBACK (on_pref_macro_changed), self);
  return e;
}

/* Rows of one bank, packed as an untitled group — the caller puts both
 * banks in a GtkStack behind a Run/S&P switcher so it is always explicit
 * which bank is being edited. */
static GtkWidget *
macro_bank_rows (LogflWindow *self, LogflMacroBankId bank)
{
  AdwPreferencesGroup *grp =
      ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  for (guint i = 0; i < LOGFL_MACRO_N_KEYS; i++)
    {
      const LogflMacroKey *k =
          logfl_macro_set_key (&self->settings.macros, bank, i);
      GtkWidget *xrow = adw_expander_row_new ();
      /* Captions/templates are user text — never Pango markup. */
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (xrow), FALSE);
      adw_expander_row_add_row (
          ADW_EXPANDER_ROW (xrow),
          pref_macro_entry (self, "Caption", k->caption, bank, i, TRUE,
                            xrow));
      if (logfl_macro_index_is_stop (i))
        adw_expander_row_set_subtitle (
            ADW_EXPANDER_ROW (xrow),
            "Esc — always stops the keyer; caption only");
      else
        adw_expander_row_add_row (
            ADW_EXPANDER_ROW (xrow),
            pref_macro_entry (self, "CW text", k->tmpl, bank, i, FALSE,
                              xrow));
      pref_macro_retitle (ADW_EXPANDER_ROW (xrow), k, i);
      adw_preferences_group_add (grp, xrow);
    }
  return GTK_WIDGET (grp);
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
      "Macro keys are editable below, or right-click them on the bar.",
      NULL));
  GtkWidget *esm_row = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (esm_row),
                                 "Enter sends message");
  adw_switch_row_set_active (ADW_SWITCH_ROW (esm_row),
                             self->settings.esm_enabled);
  adw_preferences_group_add (cgrp, esm_row);
  adw_preferences_page_add (p_msg, cgrp);

  /* Both banks behind an explicit Run/S&P switcher; opens on the bank
   * that is active on the macro bar. */
  AdwPreferencesGroup *mgrp = ADW_PREFERENCES_GROUP (g_object_new (
      ADW_TYPE_PREFERENCES_GROUP,
      "title", "Macro banks",
      "description",
      "Tokens: {MYCALL} {CALL} {RST} {NR} {EXCH} — ! is short for {CALL}, "
      "{NR}/{EXCH} are the contest serial and my sent exchange. "
      "Empty CW text = unused slot.",
      NULL));
  GtkWidget *mstack = gtk_stack_new ();
  gtk_stack_set_vhomogeneous (GTK_STACK (mstack), FALSE);
  gtk_stack_set_transition_type (GTK_STACK (mstack),
                                 GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_titled (GTK_STACK (mstack),
                        macro_bank_rows (self, LOGFL_MACRO_BANK_RUN),
                        "run", "Run");
  gtk_stack_add_titled (GTK_STACK (mstack),
                        macro_bank_rows (self, LOGFL_MACRO_BANK_SNP),
                        "snp", "S&P");
  gtk_stack_set_visible_child_name (
      GTK_STACK (mstack),
      self->settings.macro_bank == LOGFL_MACRO_BANK_SNP ? "snp" : "run");
  GtkWidget *msw = gtk_stack_switcher_new ();
  gtk_stack_switcher_set_stack (GTK_STACK_SWITCHER (msw),
                                GTK_STACK (mstack));
  gtk_widget_set_halign (msw, GTK_ALIGN_CENTER);
  GtkWidget *mbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_box_append (GTK_BOX (mbox), msw);
  gtk_box_append (GTK_BOX (mbox), mstack);
  adw_preferences_group_add (mgrp, mbox);
  adw_preferences_page_add (p_msg, mgrp);

  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dlg), p_msg);

  /* --- WSJT-X / JTDX UDP ----------------------------------------------- */
  AdwPreferencesPage *p_wsjtx = ADW_PREFERENCES_PAGE (g_object_new (
      ADW_TYPE_PREFERENCES_PAGE,
      "title", "WSJT-X",
      "icon-name", "network-wireless-symbolic",
      NULL));
  AdwPreferencesGroup *wgrp = ADW_PREFERENCES_GROUP (g_object_new (
      ADW_TYPE_PREFERENCES_GROUP,
      "title", "UDP server",
      "description",
      "Listen for QSO Logged from WSJT-X / JTDX "
      "(Settings → Reporting → UDP Server). "
      "Default 127.0.0.1:2237. Worked-B4 highlights the DX call in Band Activity.",
      NULL));
  GtkWidget *wsjtx_en = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (wsjtx_en), "Enabled");
  adw_switch_row_set_active (ADW_SWITCH_ROW (wsjtx_en),
                             self->settings.wsjtx_enabled);
  adw_preferences_group_add (wgrp, wsjtx_en);
  GtkWidget *wsjtx_port = adw_spin_row_new_with_range (1, 65535, 1);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (wsjtx_port), "Port");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (wsjtx_port),
                               "WSJT-X default 2237");
  adw_spin_row_set_value (
      ADW_SPIN_ROW (wsjtx_port),
      self->settings.wsjtx_port ? self->settings.wsjtx_port
                                : LOGFL_WSJTX_DEFAULT_PORT);
  adw_preferences_group_add (wgrp, wsjtx_port);
  adw_preferences_page_add (p_wsjtx, wgrp);
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (dlg), p_wsjtx);

  /* Keep the page switcher in the header (sdr-for-linux does the same). */
  adw_dialog_set_content_width (dlg, 640);
  adw_dialog_set_content_height (dlg, 480);

  g_object_set_data (G_OBJECT (dlg), "tci-host", host_row);
  g_object_set_data (G_OBJECT (dlg), "tci-port", port_row);
  g_object_set_data (G_OBJECT (dlg), "station-call", call_row);
  g_object_set_data (G_OBJECT (dlg), "esm", esm_row);
  g_object_set_data (G_OBJECT (dlg), "wsjtx-en", wsjtx_en);
  g_object_set_data (G_OBJECT (dlg), "wsjtx-port", wsjtx_port);
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

/* --- contests (M9) ------------------------------------------------------ */

/* Next sent serial for the active contest → cached + shown in the entry
 * row. 0 when the contest has no serial (or no contest is active). */
static void
refresh_serial (LogflWindow *self)
{
  self->next_serial = 0;
  if (self->store && self->contest && self->exch_def
      && self->exch_def->tx_serial)
    {
      guint n = 1;
      if (logfl_store_serial_next (self->store, self->contest->id, &n, NULL))
        self->next_serial = n;
    }
  if (self->serial_value)
    {
      char *s;
      if (self->next_serial)
        s = logfl_exch_serial_format (self->next_serial);
      else if (self->contest && self->contest->my_exch
               && *self->contest->my_exch)
        s = g_strdup (self->contest->my_exch);
      else
        s = g_strdup ("");
      gtk_editable_set_text (GTK_EDITABLE (self->serial_value), s);
      g_free (s);
    }
}

/* Rebuild the entry-row exchange fields from the active template. */
static void
rebuild_exch_fields (LogflWindow *self)
{
  if (!self->exch_box)
    return;
  GtkWidget *c;
  while ((c = gtk_widget_get_first_child (self->exch_box)) != NULL)
    gtk_box_remove (GTK_BOX (self->exch_box), c);
  if (self->exch_entries)
    g_ptr_array_set_size (self->exch_entries, 0);
  self->serial_value = NULL;

  gtk_widget_set_visible (self->exch_box, self->exch_def != NULL);
  if (!self->exch_def)
    return;

  /* What goes out next: the serial, or the static exchange for contests
   * without one. Prefilled but editable — a mis-sent number can be fixed
   * before logging; the prefill refreshes after every logged QSO. */
  self->serial_value = mk_entry (self, 6, NULL);
  gtk_widget_add_css_class (self->serial_value, "numeric");
  gtk_box_append (GTK_BOX (self->exch_box),
                  labeled ("Sent", self->serial_value));
  for (guint i = 0; i < self->exch_def->fields->len; i++)
    {
      const LogflExchField *f = self->exch_def->fields->pdata[i];
      GtkWidget *e = mk_entry (self, 7, NULL);
      if (f->required)
        gtk_widget_set_tooltip_text (e, "Contest exchange (required)");
      gtk_box_append (GTK_BOX (self->exch_box), labeled (f->label, e));
      g_ptr_array_add (self->exch_entries, e);
    }
}

static void
rebuild_contest_menu (LogflWindow *self)
{
  if (!self->contest_btn)
    return;

  GMenu *menu = g_menu_new ();
  GMenu *sw = g_menu_new ();
  GMenuItem *it = g_menu_item_new ("Main log", NULL);
  g_menu_item_set_action_and_target_value (it, "win.contest-switch",
                                           g_variant_new_int64 (0));
  g_menu_append_item (sw, it);
  g_object_unref (it);
  if (self->store)
    {
      GPtrArray *l = logfl_store_contest_list (self->store, NULL);
      for (guint i = 0; l && i < l->len; i++)
        {
          const LogflContest *c = l->pdata[i];
          it = g_menu_item_new (c->name, NULL);
          g_menu_item_set_action_and_target_value (
              it, "win.contest-switch", g_variant_new_int64 (c->id));
          g_menu_append_item (sw, it);
          g_object_unref (it);
        }
      g_clear_pointer (&l, g_ptr_array_unref);
    }
  g_menu_append_section (menu, NULL, G_MENU_MODEL (sw));
  g_object_unref (sw);

  GMenu *mgmt = g_menu_new ();
  g_menu_append (mgmt, "_New contest…", "win.contest-new");
  g_menu_append (mgmt, "_Manage contests…", "win.contest-manage");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (mgmt));
  g_object_unref (mgmt);

  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (self->contest_btn),
                                  G_MENU_MODEL (menu));
  g_object_unref (menu);
}

static void
contest_ui_refresh (LogflWindow *self)
{
  if (self->contest_btn)
    gtk_menu_button_set_label (GTK_MENU_BUTTON (self->contest_btn),
                               self->contest ? self->contest->name
                                             : "Main log");
  rebuild_contest_menu (self);
  rebuild_exch_fields (self);
  refresh_serial (self);
  /* Contest operating: Name/Comment are dead weight in the entry row —
   * hide their labeled wrappers while a contest is active. */
  if (self->name && gtk_widget_get_parent (self->name))
    gtk_widget_set_visible (gtk_widget_get_parent (self->name),
                            self->contest == NULL);
  if (self->comment && gtk_widget_get_parent (self->comment))
    gtk_widget_set_visible (gtk_widget_get_parent (self->comment),
                            self->contest == NULL);
  if (self->col_stx)
    gtk_column_view_column_set_visible (self->col_stx,
                                        self->contest != NULL);
  if (self->col_exch)
    {
      gtk_column_view_column_set_visible (self->col_exch,
                                          self->contest != NULL);
      /* With Name/Comment gone, Rcvd takes the leftover width. */
      gtk_column_view_column_set_expand (self->col_exch,
                                         self->contest != NULL);
    }
  if (self->col_name)
    gtk_column_view_column_set_visible (self->col_name,
                                        self->contest == NULL);
  if (self->col_comment)
    gtk_column_view_column_set_visible (self->col_comment,
                                        self->contest == NULL);
  update_wb4 (self);
}

/* Load contest id into self (template parsed); no UI refresh, no save.
 * A broken template drops back to the main log rather than half-working. */
static void
contest_load (LogflWindow *self, gint64 id)
{
  g_clear_pointer (&self->contest, logfl_contest_free);
  g_clear_pointer (&self->exch_def, logfl_exch_def_free);
  if (id <= 0 || !self->store)
    return;

  GError *err = NULL;
  self->contest = logfl_store_contest_get (self->store, id, &err);
  if (!self->contest)
    {
      g_clear_error (&err);
      return;
    }
  self->exch_def = logfl_exch_def_parse (self->contest->exch_def, &err);
  if (!self->exch_def)
    {
      if (self->toasts)
        toast (self, "Broken exchange template in %s: %s",
               self->contest->name, err ? err->message : "?");
      g_clear_error (&err);
      g_clear_pointer (&self->contest, logfl_contest_free);
    }
}

static void
contest_set_active (LogflWindow *self, gint64 id)
{
  contest_load (self, id);
  self->settings.active_contest = self->contest ? self->contest->id : 0;
  logfl_settings_save (&self->settings);
  contest_ui_refresh (self);
  reload (self);
  toast_short (self, "%s", self->contest ? self->contest->name : "Main log");
}

static void
act_contest_switch (GSimpleAction *action, GVariant *param,
                    gpointer user_data)
{
  (void) action;
  contest_set_active (LOGFL_WINDOW (user_data), g_variant_get_int64 (param));
}

/* --- new contest dialog -------------------------------------------------- */

/* Prefill ADIF id, my-exchange hint and the definition from a preset. */
static void
contest_preset_fill (AdwDialog *dlg, guint idx)
{
  guint n = 0;
  const LogflContestPreset *p = logfl_contest_presets (&n);
  if (idx >= n)
    return;
  GtkWidget *adif = g_object_get_data (G_OBJECT (dlg), "adif");
  GtkWidget *my = g_object_get_data (G_OBJECT (dlg), "myexch");
  GtkTextView *def = g_object_get_data (G_OBJECT (dlg), "def");
  gtk_editable_set_text (GTK_EDITABLE (adif),
                         p[idx].adif_id ? p[idx].adif_id : "");
  gtk_entry_set_placeholder_text (GTK_ENTRY (my),
                                  p[idx].my_exch_hint ? p[idx].my_exch_hint
                                                      : "");
  gtk_text_buffer_set_text (gtk_text_view_get_buffer (def),
                            p[idx].exch_def, -1);
}

static void
on_contest_preset_changed (GObject *dd, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  contest_preset_fill (user_data,
                       gtk_drop_down_get_selected (GTK_DROP_DOWN (dd)));
}

static void
on_contest_new_response (GObject *source, GAsyncResult *res,
                         gpointer user_data)
{
  LogflWindow *self = user_data;
  AdwAlertDialog *dlg = ADW_ALERT_DIALOG (source);
  const char *resp = adw_alert_dialog_choose_finish (dlg, res);
  if (!g_str_equal (resp, "create"))
    return;

  GtkWidget *name = g_object_get_data (G_OBJECT (dlg), "name");
  GtkWidget *adif = g_object_get_data (G_OBJECT (dlg), "adif");
  GtkWidget *my = g_object_get_data (G_OBJECT (dlg), "myexch");
  GtkTextView *defv = g_object_get_data (G_OBJECT (dlg), "def");

  const char *nm = entry_text (name);
  if (!*nm)
    {
      toast (self, "Contest not created — it needs a name");
      return;
    }

  GtkTextBuffer *buf = gtk_text_view_get_buffer (defv);
  GtkTextIter a, b;
  gtk_text_buffer_get_bounds (buf, &a, &b);
  char *def_text = gtk_text_buffer_get_text (buf, &a, &b, FALSE);

  GError *err = NULL;
  LogflExchDef *parsed = logfl_exch_def_parse (def_text, &err);
  if (!parsed)
    {
      toast (self, "Contest not created — bad exchange definition: %s",
             err ? err->message : "?");
      g_clear_error (&err);
      g_free (def_text);
      return;
    }
  logfl_exch_def_free (parsed);

  LogflContest *c = logfl_contest_new ();
  c->name = g_strdup (nm);
  c->adif_id = g_strdup (entry_text (adif));
  c->my_exch = g_strdup (entry_text (my));
  c->exch_def = def_text;
  if (!logfl_store_contest_add (self->store, c, &err))
    {
      toast (self, "Contest not created: %s", err->message);
      g_clear_error (&err);
      logfl_contest_free (c);
      return;
    }
  contest_set_active (self, c->id);
  logfl_contest_free (c);
}

static void
act_contest_new (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void) action;
  (void) param;
  LogflWindow *self = user_data;
  if (!self->store)
    {
      toast (self, "Log store is not open");
      return;
    }

  guint n = 0;
  const LogflContestPreset *presets = logfl_contest_presets (&n);
  const char **names = g_new0 (const char *, n + 1);
  for (guint i = 0; i < n; i++)
    names[i] = presets[i].name;

  AdwDialog *dlg = adw_alert_dialog_new ("New contest", NULL);

  GtkWidget *name = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (name), "CQ WW CW 2026");
  gtk_widget_set_hexpand (name, TRUE);
  GtkWidget *preset_dd = gtk_drop_down_new_from_strings (names);
  g_free (names);
  GtkWidget *adif = gtk_entry_new ();
  gtk_widget_set_hexpand (adif, TRUE);
  GtkWidget *my = gtk_entry_new ();
  gtk_widget_set_hexpand (my, TRUE);

  GtkWidget *defv = gtk_text_view_new ();
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (defv), TRUE);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (defv), 6);
  gtk_text_view_set_right_margin (GTK_TEXT_VIEW (defv), 6);
  gtk_text_view_set_top_margin (GTK_TEXT_VIEW (defv), 4);
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (defv), 4);
  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), defv);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroller),
                                              150);
  gtk_widget_add_css_class (scroller, "card");

  GtkWidget *row1 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (row1), labeled ("Name", name));
  gtk_box_append (GTK_BOX (row1), labeled ("Preset", preset_dd));
  GtkWidget *row2 = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (row2), labeled ("ADIF CONTEST_ID", adif));
  gtk_box_append (GTK_BOX (row2), labeled ("My exchange", my));

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_box_append (GTK_BOX (box), row1);
  gtk_box_append (GTK_BOX (box), row2);
  gtk_box_append (GTK_BOX (box),
                  labeled ("Exchange definition (received fields → ADIF)",
                           scroller));
  adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dlg), box);

  g_object_set_data (G_OBJECT (dlg), "name", name);
  g_object_set_data (G_OBJECT (dlg), "adif", adif);
  g_object_set_data (G_OBJECT (dlg), "myexch", my);
  g_object_set_data (G_OBJECT (dlg), "def", defv);
  g_signal_connect (preset_dd, "notify::selected",
                    G_CALLBACK (on_contest_preset_changed), dlg);
  contest_preset_fill (dlg, 0);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", "Cancel", "create", "Create",
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "create",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "create");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg), GTK_WIDGET (self), NULL,
                           on_contest_new_response, self);
}

/* --- manage / delete ----------------------------------------------------- */

static void
on_contest_delete_response (GObject *source, GAsyncResult *res,
                            gpointer user_data)
{
  LogflWindow *self = user_data;
  const char *resp =
      adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source), res);
  gint64 id = self->pending_contest_delete;
  self->pending_contest_delete = 0;
  gboolean purge = g_str_equal (resp, "purge");
  if (id <= 0 || (!purge && !g_str_equal (resp, "keep")))
    return;

  guint n = 0;
  GError *err = NULL;
  if (!logfl_store_contest_delete (self->store, id, purge, &n, &err))
    {
      toast (self, "Delete failed: %s", err->message);
      g_clear_error (&err);
      return;
    }
  if (purge)
    toast (self, "Contest deleted with %u QSO", n);
  else
    toast (self, "Contest deleted — %u QSO kept in the main log", n);

  if (self->contest && self->contest->id == id)
    contest_set_active (self, 0);   /* refreshes UI + reload + settings */
  else
    {
      contest_ui_refresh (self);
      reload (self);
    }
}

static void
confirm_contest_delete (LogflWindow *self, gint64 id, const char *name)
{
  LogflStoreStats st = { 0 };
  logfl_store_contest_stats (self->store, id, &st, NULL);
  self->pending_contest_delete = id;

  AdwDialog *dlg = adw_alert_dialog_new ("Delete contest?", NULL);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (dlg),
      "“%s” holds %u QSO. Keep them in the main log, or delete them too?",
      name, st.n_qso);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", "Cancel",
                                  "keep", "Delete, keep QSOs",
                                  "purge", "Delete with QSOs", NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "purge",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "cancel");
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg), GTK_WIDGET (self), NULL,
                           on_contest_delete_response, self);
}

static void
on_contest_row_delete (GtkButton *btn, gpointer user_data)
{
  LogflWindow *self = user_data;
  gint64 *idp = g_object_get_data (G_OBJECT (btn), "cid");
  const char *cname = g_object_get_data (G_OBJECT (btn), "cname");
  AdwDialog *mgr = g_object_get_data (G_OBJECT (btn), "mgr");
  if (!idp)
    return;
  /* Copy before closing the manager — closing tears the button down. */
  gint64 id = *idp;
  char *nm = g_strdup (cname ? cname : "?");
  if (mgr)
    adw_dialog_close (mgr);
  confirm_contest_delete (self, id, nm);
  g_free (nm);
}

static void
act_contest_manage (GSimpleAction *action, GVariant *param,
                    gpointer user_data)
{
  (void) action;
  (void) param;
  LogflWindow *self = user_data;
  if (!self->store)
    {
      toast (self, "Log store is not open");
      return;
    }
  GError *err = NULL;
  GPtrArray *l = logfl_store_contest_list (self->store, &err);
  if (!l)
    {
      toast (self, "Cannot list contests: %s", err->message);
      g_clear_error (&err);
      return;
    }

  AdwDialog *dlg = adw_alert_dialog_new ("Contests", NULL);
  if (l->len == 0)
    adw_alert_dialog_set_body (ADW_ALERT_DIALOG (dlg), "No contests yet.");
  else
    {
      GtkWidget *list = gtk_list_box_new ();
      gtk_list_box_set_selection_mode (GTK_LIST_BOX (list),
                                       GTK_SELECTION_NONE);
      gtk_widget_add_css_class (list, "boxed-list");
      for (guint i = 0; i < l->len; i++)
        {
          const LogflContest *c = l->pdata[i];
          GtkWidget *row = adw_action_row_new ();
          adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row),
                                              FALSE);
          adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), c->name);

          LogflStoreStats st = { 0 };
          logfl_store_contest_stats (self->store, c->id, &st, NULL);
          GDateTime *dt = g_date_time_new_from_unix_utc (c->created);
          char *when = dt ? g_date_time_format (dt, "%d.%m.%Y") : NULL;
          char *sub = g_strdup_printf ("%u QSO · created %s", st.n_qso,
                                       when ? when : "?");
          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), sub);
          g_free (sub);
          g_free (when);
          if (dt)
            g_date_time_unref (dt);

          GtkWidget *del =
              gtk_button_new_from_icon_name ("user-trash-symbolic");
          gtk_widget_add_css_class (del, "flat");
          gtk_widget_set_valign (del, GTK_ALIGN_CENTER);
          gtk_widget_set_tooltip_text (del, "Delete contest…");
          g_object_set_data_full (G_OBJECT (del), "cid",
                                  g_memdup2 (&c->id, sizeof c->id), g_free);
          g_object_set_data_full (G_OBJECT (del), "cname",
                                  g_strdup (c->name), g_free);
          g_object_set_data (G_OBJECT (del), "mgr", dlg);
          g_signal_connect (del, "clicked",
                            G_CALLBACK (on_contest_row_delete), self);
          adw_action_row_add_suffix (ADW_ACTION_ROW (row), del);
          gtk_list_box_append (GTK_LIST_BOX (list), row);
        }
      adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dlg), list);
    }
  g_ptr_array_unref (l);

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "close", "Close", NULL);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "close");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dlg), "close");
  adw_dialog_present (dlg, GTK_WIDGET (self));
}

/* --- QSO table (inline cell edit via GtkEditableLabel) ----------------- */

/* Display string for a column (caller frees). Matches what the cell shows. */
static char *
cell_display_text (int col, const LogflQso *q)
{
  switch (col)
    {
    case COL_UTC:
      {
        GDateTime *dt = g_date_time_new_from_unix_utc (q->ts);
        char *s = g_date_time_format (dt, "%d.%m.%y %H:%M");
        g_date_time_unref (dt);
        return s;
      }
    case COL_CALL:
      return g_strdup (q->call ? q->call : "");
    case COL_BAND:
      return g_strdup (q->band ? q->band : "");
    case COL_FREQ:
      return fmt_freq (q->freq);
    case COL_MODE:
      if (q->submode && *q->submode)
        return g_strdup_printf ("%s/%s",
                                q->mode ? q->mode : "", q->submode);
      return g_strdup (q->mode ? q->mode : "");
    case COL_RST_S:
      return g_strdup (q->rst_sent ? q->rst_sent : "");
    case COL_RST_R:
      return g_strdup (q->rst_rcvd ? q->rst_rcvd : "");
    case COL_NAME:
      return g_strdup (q->name ? q->name : "");
    case COL_COMMENT:
      return g_strdup (q->comment ? q->comment : "");
    case COL_STX:
      {
        /* Serial and text part both shown — the cell is editable and what
         * it displays is exactly what a commit stores. */
        char *nr = q->stx > 0
                       ? logfl_exch_serial_format ((guint) q->stx) : NULL;
        const char *txt = q->stx_string ? q->stx_string : "";
        char *s = g_strdup_printf ("%s%s%s", nr ? nr : "",
                                   nr && *txt ? " " : "", txt);
        g_free (nr);
        return s;
      }
    case COL_EXCH:
      {
        char *nr = q->srx > 0
                       ? g_strdup_printf ("%" G_GINT64_FORMAT, q->srx)
                       : NULL;
        const char *txt = q->srx_string ? q->srx_string : "";
        char *s = g_strdup_printf ("%s%s%s", nr ? nr : "",
                                   nr && *txt ? " " : "", txt);
        g_free (nr);
        return s;
      }
    default:
      return g_strdup ("");
    }
}

/* Text to prefill the cell entry with — the display string is the raw
 * value for every column. */
static char *
cell_edit_text (int col, const LogflQso *q)
{
  return cell_display_text (col, q);
}

/* Exchange cell text → serial + uppercased text remainder. WYSIWYG: the
 * first digits-only token becomes the serial, everything else joins the
 * text part; either output may end up unset. */
static void
parse_exch_cell (const char *raw, gint64 *serial, char **text_out)
{
  *serial = 0;
  g_clear_pointer (text_out, g_free);
  char **tok = g_strsplit_set (raw, " \t", -1);
  GString *rest = g_string_new (NULL);
  for (char **t = tok; *t; t++)
    {
      if (!**t)
        continue;
      gboolean digits = TRUE;
      for (const char *c = *t; *c; c++)
        if (!g_ascii_isdigit (*c))
          {
            digits = FALSE;
            break;
          }
      if (digits && *serial == 0)
        *serial = g_ascii_strtoll (*t, NULL, 10);
      else
        {
          if (rest->len)
            g_string_append_c (rest, ' ');
          char *up = g_ascii_strup (*t, -1);
          g_string_append (rest, up);
          g_free (up);
        }
    }
  g_strfreev (tok);
  if (rest->len)
    *text_out = g_string_free (rest, FALSE);
  else
    g_string_free (rest, TRUE);
}

/* True if s is empty or a placeholder dash (ASCII or em dash). */
static gboolean
rst_part_empty (const char *s)
{
  if (!s || !*s)
    return TRUE;
  if (strcmp (s, "-") == 0 || strcmp (s, "—") == 0 || strcmp (s, "–") == 0)
    return TRUE;
  return FALSE;
}

/* Apply one cell's text onto q. On failure sets error and leaves q partially
 * modified only for the fields that were assigned before the failure — caller
 * should discard q (we always work on a store_get copy). */
static gboolean
apply_cell_to_qso (LogflQso *q, int col, const char *raw, GError **error)
{
  char *text = g_strdup (raw ? raw : "");
  g_strstrip (text);

  switch (col)
    {
    case COL_UTC:
      {
        int d = 0, m = 0, y = 0, H = 0, M = 0;
        if (sscanf (text, "%d.%d.%d %d:%d", &d, &m, &y, &H, &M) != 5)
          {
            g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID,
                         "UTC must be DD.MM.YY HH:MM");
            g_free (text);
            return FALSE;
          }
        if (y < 100)
          y += 2000;
        if (m < 1 || m > 12 || d < 1 || d > 31 || H < 0 || H > 23
            || M < 0 || M > 59)
          {
            g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID,
                         "invalid UTC date/time");
            g_free (text);
            return FALSE;
          }
        GDateTime *dt = g_date_time_new_utc (y, m, d, H, M, 0);
        if (!dt)
          {
            g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID,
                         "invalid UTC date/time");
            g_free (text);
            return FALSE;
          }
        q->ts = g_date_time_to_unix (dt);
        g_date_time_unref (dt);
        break;
      }
    case COL_CALL:
      if (!*text)
        {
          g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID,
                       "callsign required");
          g_free (text);
          return FALSE;
        }
      g_free (q->call);
      q->call = g_steal_pointer (&text);
      break;
    case COL_BAND:
      {
        if (!*text)
          {
            g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID,
                         "band required");
            g_free (text);
            return FALSE;
          }
        g_free (q->band);
        q->band = g_steal_pointer (&text);
        /* Keep freq inside the new band — reseed mid-band on mismatch. */
        const char *fb =
            q->freq > 0 ? logfl_adif_band_for_freq (q->freq) : NULL;
        if (!fb || g_ascii_strcasecmp (fb, q->band) != 0)
          q->freq = logfl_adif_freq_for_band (q->band);
        break;
      }
    case COL_FREQ:
      {
        if (!*text)
          {
            q->freq = 0;
            break;
          }
        g_strdelimit (text, ",", '.');
        char *end = NULL;
        double mhz = g_ascii_strtod (text, &end);
        if (end == text || mhz < 0)
          {
            g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID,
                         "invalid frequency");
            g_free (text);
            return FALSE;
          }
        q->freq = mhz;
        /* Frequency is the source of truth — rederive band when known. */
        if (mhz > 0)
          {
            const char *nb = logfl_adif_band_for_freq (mhz);
            if (nb)
              {
                g_free (q->band);
                q->band = g_strdup (nb);
              }
          }
        break;
      }
    case COL_MODE:
      {
        if (!*text)
          {
            g_set_error (error, LOGFL_STORE_ERROR, LOGFL_STORE_ERROR_INVALID,
                         "mode required");
            g_free (text);
            return FALSE;
          }
        char *slash = strchr (text, '/');
        g_free (q->mode);
        g_free (q->submode);
        if (slash && slash != text && slash[1])
          {
            *slash = '\0';
            q->mode = g_strdup (text);
            q->submode = g_strdup (slash + 1);
            g_strstrip (q->mode);
            g_strstrip (q->submode);
          }
        else
          {
            q->mode = g_steal_pointer (&text);
            q->submode = NULL;
          }
        break;
      }
    case COL_RST_S:
      g_free (q->rst_sent);
      q->rst_sent = rst_part_empty (text) ? NULL : g_strdup (text);
      break;
    case COL_RST_R:
      g_free (q->rst_rcvd);
      q->rst_rcvd = rst_part_empty (text) ? NULL : g_strdup (text);
      break;
    case COL_NAME:
      g_free (q->name);
      q->name = *text ? g_steal_pointer (&text) : NULL;
      break;
    case COL_COMMENT:
      g_free (q->comment);
      q->comment = *text ? g_steal_pointer (&text) : NULL;
      break;
    case COL_STX:
      parse_exch_cell (text, &q->stx, &q->stx_string);
      break;
    case COL_EXCH:
      parse_exch_cell (text, &q->srx, &q->srx_string);
      break;
    default:
      g_free (text);
      return TRUE;
    }

  g_free (text);
  return TRUE;
}

/* Commit edited cell text to the store and update the in-memory row.
 * Painting is the caller's job — the edited cell directly, siblings via
 * row_repaint_siblings (never a model items-changed, which would rebuild
 * and visibly flash the whole row). */
static gboolean
commit_cell_edit (LogflWindow *self, LogflQsoRow *row, int col,
                  const char *new_text, gboolean *out_changed)
{
  if (out_changed)
    *out_changed = FALSE;
  if (!self || !self->store || !row)
    return FALSE;

  const LogflQso *cur = logfl_qso_row_qso (row);
  if (!cur || cur->id <= 0)
    return FALSE;

  char *old_disp = cell_edit_text (col, cur);
  char *nt_strip = g_strdup (new_text ? new_text : "");
  g_strstrip (nt_strip);
  gboolean same = (g_strcmp0 (old_disp, nt_strip) == 0);
  g_free (old_disp);
  g_free (nt_strip);
  if (same)
    return TRUE;

  GError *err = NULL;
  LogflQso *q = logfl_store_get (self->store, cur->id, &err);
  if (!q)
    {
      toast (self, "Edit failed: %s",
             err ? err->message : "not found");
      g_clear_error (&err);
      return FALSE;
    }

  if (!apply_cell_to_qso (q, col, new_text, &err))
    {
      toast (self, "Not saved: %s", err->message);
      g_clear_error (&err);
      logfl_qso_free (q);
      return FALSE;
    }

  if (!logfl_store_update (self->store, q, &err))
    {
      toast (self, "Not saved: %s", err->message);
      g_clear_error (&err);
      logfl_qso_free (q);
      return FALSE;
    }

  logfl_qso_row_replace (row, q);
  if (out_changed)
    *out_changed = TRUE;

  update_subtitle (self);
  /* Editing the highest sent serial moves the next-serial prefill. */
  refresh_serial (self);
  return TRUE;
}

/* Cell root is a GtkBox with a display GtkLabel + a hidden GtkEntry that a
 * single click swaps in. (GtkEditableLabel was worse: it stole the first
 * click; a double-click-only gesture made the hover highlight look like a
 * pointless flash.) */

static GtkWidget *
cell_label (GtkWidget *box)
{
  return g_object_get_data (G_OBJECT (box), "logfl-label");
}

static GtkWidget *
cell_entry (GtkWidget *box)
{
  return g_object_get_data (G_OBJECT (box), "logfl-entry");
}

static gboolean
cell_is_editing (GtkWidget *box)
{
  GtkWidget *entry = cell_entry (box);
  return entry && gtk_widget_get_visible (entry);
}

static void
cell_show_label (GtkWidget *box)
{
  GtkWidget *label = cell_label (box);
  GtkWidget *entry = cell_entry (box);
  if (entry)
    gtk_widget_set_visible (entry, FALSE);
  if (label)
    gtk_widget_set_visible (label, TRUE);
}

static void
cell_set_display (GtkWidget *box, const char *txt)
{
  GtkWidget *label = cell_label (box);
  if (label)
    gtk_label_set_text (GTK_LABEL (label), txt ? txt : "");
}

static void
cell_repaint_from_row (GtkWidget *box)
{
  LogflQsoRow *row = g_object_get_data (G_OBJECT (box), "logfl-row");
  if (!row || cell_is_editing (box))
    return;
  int col = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (box), "logfl-col"));
  char *txt = cell_display_text (col, logfl_qso_row_qso (row));
  cell_set_display (box, txt);
  g_free (txt);
}

/* Repaint every cell of the row that contains box straight from the row
 * data. Widget path: box → GtkColumnViewCell → row widget → sibling cells;
 * cells are recognized by the "logfl-label" marker, so a GTK layout change
 * degrades to repainting nothing. */
static void
row_repaint_siblings (GtkWidget *box)
{
  GtkWidget *cell = gtk_widget_get_parent (box);
  GtkWidget *rw = cell ? gtk_widget_get_parent (cell) : NULL;
  if (!rw)
    return;
  for (GtkWidget *c = gtk_widget_get_first_child (rw); c != NULL;
       c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *b = gtk_widget_get_first_child (c);
      if (b && cell_label (b))
        cell_repaint_from_row (b);
    }
}

static void
cell_end_edit (GtkWidget *box, gboolean commit)
{
  if (!cell_is_editing (box))
    return;

  LogflWindow *self = g_object_get_data (G_OBJECT (box), "logfl-win");
  LogflQsoRow *row = g_object_get_data (G_OBJECT (box), "logfl-row");
  int col = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (box), "logfl-col"));
  GtkWidget *entry = cell_entry (box);

  /* Leave edit mode in the window tracker first so CAPTURE Esc / F-keys
   * and list rebind do not treat this cell as still open. */
  if (self && self->cell_edit_box == box)
    self->cell_edit_box = NULL;

  gboolean changed = FALSE;
  if (commit && self && row && entry)
    {
      const char *text = gtk_editable_get_text (GTK_EDITABLE (entry));
      if (!commit_cell_edit (self, row, col, text, &changed))
        {
          /* Failed — keep previous stored value on the label. */
        }
    }

  /* Always paint the label from the (possibly updated) row *now*, then
   * swap entry → label. Do not wait for list items-changed / rebind. */
  if (row)
    {
      char *txt = cell_display_text (col, logfl_qso_row_qso (row));
      cell_set_display (box, txt);
      g_free (txt);
    }
  cell_show_label (box);

  /* Sibling cells can show derived fields (MHz ↔ Band) — repaint them in
   * place from the updated row data. */
  if (changed)
    row_repaint_siblings (box);
}

static void
cell_begin_edit (GtkWidget *box)
{
  LogflWindow *self = g_object_get_data (G_OBJECT (box), "logfl-win");
  LogflQsoRow *row = g_object_get_data (G_OBJECT (box), "logfl-row");
  int col = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (box), "logfl-col"));
  GtkWidget *label = cell_label (box);
  GtkWidget *entry = cell_entry (box);
  if (!row || !entry || !label)
    return;
  if (cell_is_editing (box))
    return;

  /* One cell at a time — discard the previous if any (only Enter saves). */
  if (self && self->cell_edit_box && self->cell_edit_box != box)
    cell_end_edit (self->cell_edit_box, FALSE);

  char *txt = cell_edit_text (col, logfl_qso_row_qso (row));
  gtk_editable_set_text (GTK_EDITABLE (entry), txt ? txt : "");
  g_free (txt);

  gtk_widget_set_visible (label, FALSE);
  gtk_widget_set_visible (entry, TRUE);
  if (self)
    self->cell_edit_box = box;
  gtk_widget_grab_focus (entry);
  gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
}

/* Idle: focus left the entry — discard the edit (only Enter commits).
 * Holds a ref on box; skip once the widget is off the tree (window closing —
 * the stored "logfl-win" pointer must not be dereferenced then). */
static gboolean
cell_end_edit_idle (gpointer data)
{
  GtkWidget *box = data;
  if (gtk_widget_get_root (box) != NULL && cell_is_editing (box))
    {
      GtkWidget *entry = cell_entry (box);
      if (!(entry && gtk_widget_has_focus (entry)))
        cell_end_edit (box, FALSE);
    }
  g_object_unref (box);
  return G_SOURCE_REMOVE;
}

static void
on_cell_entry_activate (GtkEntry *entry, gpointer user_data)
{
  (void) entry;
  cell_end_edit (user_data, TRUE);
}

static void
on_cell_entry_focus_leave (GtkEventControllerFocus *ctl, gpointer user_data)
{
  (void) ctl;
  GtkWidget *box = user_data;
  if (!cell_is_editing (box))
    return;
  /* Defer so activate/Escape handlers run first. */
  g_idle_add (cell_end_edit_idle, g_object_ref (box));
}

static gboolean
on_cell_entry_key (GtkEventControllerKey *ctl, guint keyval, guint keycode,
                   GdkModifierType state, gpointer user_data)
{
  (void) ctl;
  (void) keycode;
  (void) state;
  if (keyval == GDK_KEY_Escape)
    {
      cell_end_edit (user_data, FALSE);
      return TRUE;
    }
  return FALSE;
}

/* Single click opens the cell editor (the row hover highlight marks where
 * the click will land). Clicks inside an already-open entry must fall
 * through so cursor placement and text selection keep working. */
static void
on_cell_click (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
               gpointer user_data)
{
  (void) x;
  (void) y;
  if (n_press != 1)
    return;
  GtkWidget *box = user_data;
  if (!g_object_get_data (G_OBJECT (box), "logfl-row"))
    return;
  if (cell_is_editing (box))
    return;
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  cell_begin_edit (box);
}

static void
col_setup (GtkSignalListItemFactory *factory, GObject *object,
           gpointer user_data)
{
  int col = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (factory), "col"));
  LogflWindow *self = user_data;
  GtkListItem *item = GTK_LIST_ITEM (object);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (box, TRUE);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (box, "logfl-cell");

  GtkWidget *label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (label, TRUE);
  gtk_widget_set_halign (label, GTK_ALIGN_FILL);
  gtk_widget_set_valign (label, GTK_ALIGN_CENTER);

  GtkWidget *entry = gtk_entry_new ();
  gtk_widget_set_hexpand (entry, TRUE);
  gtk_widget_set_valign (entry, GTK_ALIGN_CENTER);
  gtk_widget_set_visible (entry, FALSE);
  /* Flat, compact — match label row height instead of a full form entry. */
  gtk_entry_set_has_frame (GTK_ENTRY (entry), FALSE);
  gtk_widget_add_css_class (entry, "flat");
  gtk_widget_add_css_class (entry, "logfl-cell-edit");

  gtk_box_append (GTK_BOX (box), label);
  gtk_box_append (GTK_BOX (box), entry);

  g_object_set_data (G_OBJECT (box), "logfl-win", self);
  g_object_set_data (G_OBJECT (box), "logfl-col", GINT_TO_POINTER (col));
  g_object_set_data (G_OBJECT (box), "logfl-label", label);
  g_object_set_data (G_OBJECT (box), "logfl-entry", entry);

  g_signal_connect (entry, "activate",
                    G_CALLBACK (on_cell_entry_activate), box);

  GtkEventController *focus = gtk_event_controller_focus_new ();
  g_signal_connect (focus, "leave",
                    G_CALLBACK (on_cell_entry_focus_leave), box);
  gtk_widget_add_controller (entry, focus);

  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed",
                    G_CALLBACK (on_cell_entry_key), box);
  gtk_widget_add_controller (entry, keys);

  /* Gesture on the box (covers the label) — first click starts the edit. */
  GtkGesture *click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click),
                                 GDK_BUTTON_PRIMARY);
  g_signal_connect (click, "pressed", G_CALLBACK (on_cell_click), box);
  gtk_widget_add_controller (box, GTK_EVENT_CONTROLLER (click));

  /* No focus / select / activate — only double-click gesture edits. */
  gtk_list_item_set_focusable (item, FALSE);
  gtk_list_item_set_selectable (item, FALSE);
  gtk_list_item_set_activatable (item, FALSE);
  gtk_list_item_set_child (item, box);
}

static void
col_bind (GtkSignalListItemFactory *factory, GObject *object,
          gpointer user_data)
{
  (void) user_data;
  int col = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (factory), "col"));
  GtkListItem *item = GTK_LIST_ITEM (object);
  GtkWidget *box = gtk_list_item_get_child (item);
  LogflQsoRow *row = gtk_list_item_get_item (item);
  if (!box || !row)
    return;

  g_object_set_data (G_OBJECT (box), "logfl-row", row);
  g_object_set_data (G_OBJECT (box), "logfl-col", GINT_TO_POINTER (col));

  if (cell_is_editing (box))
    return;

  char *txt = cell_display_text (col, logfl_qso_row_qso (row));
  cell_set_display (box, txt);
  g_free (txt);
}

static void
col_unbind (GtkSignalListItemFactory *factory, GObject *object,
            gpointer user_data)
{
  (void) factory;
  (void) user_data;
  GtkListItem *item = GTK_LIST_ITEM (object);
  GtkWidget *box = gtk_list_item_get_child (item);
  if (!box)
    return;

  /* Scroll-away while editing: discard — saving happens on Enter only. */
  if (cell_is_editing (box))
    cell_end_edit (box, FALSE);

  g_object_set_data (G_OBJECT (box), "logfl-row", NULL);
}

/* fixed_w > 0: preferred column width (px). expand: share leftover space.
 * Returns the column (borrowed — the view owns it). */
static GtkColumnViewColumn *
add_column (GtkColumnView *view, const char *title, int col,
            int fixed_w, gboolean expand, LogflWindow *self)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
  g_object_set_data (G_OBJECT (factory), "col", GINT_TO_POINTER (col));
  g_signal_connect (factory, "setup", G_CALLBACK (col_setup), self);
  g_signal_connect (factory, "bind", G_CALLBACK (col_bind), self);
  g_signal_connect (factory, "unbind", G_CALLBACK (col_unbind), self);
  GtkColumnViewColumn *c = gtk_column_view_column_new (title, factory);
  gtk_column_view_column_set_resizable (c, TRUE);
  if (fixed_w > 0)
    gtk_column_view_column_set_fixed_width (c, fixed_w);
  gtk_column_view_column_set_expand (c, expand);
  gtk_column_view_append_column (view, c);
  g_object_unref (c);
  return c;
}

/* App-wide CSS for the QSO table: airier rows, compact inline entry so
 * opening a cell edit does not jump the row height. */
static void
ensure_table_css (void)
{
  static gboolean loaded = FALSE;
  if (loaded)
    return;
  loaded = TRUE;

  const char *css =
      "columnview.data-table listview > row {\n"
      "  min-height: 2.6em;\n"
      "}\n"
      "columnview.data-table listview > row > cell {\n"
      "  padding: 6px 10px;\n"
      "}\n"
      "columnview.data-table .logfl-cell {\n"
      "  min-height: 1.8em;\n"
      "}\n"
      "columnview.data-table entry.logfl-cell-edit {\n"
      "  min-height: 1.8em;\n"
      "  padding-top: 2px;\n"
      "  padding-bottom: 2px;\n"
      "  padding-left: 4px;\n"
      "  padding-right: 4px;\n"
      "  margin: 0;\n"
      "  border-radius: 4px;\n"
      "  outline-offset: -1px;\n"
      "}\n"
      "columnview.data-table .logfl-cell label {\n"
      "  margin: 0;\n"
      "  padding: 2px 0;\n"
      "}\n";

  GtkCssProvider *prov = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (prov, css);
  gtk_style_context_add_provider_for_display (
      gdk_display_get_default (),
      GTK_STYLE_PROVIDER (prov),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (prov);
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
logfl_window_dispose (GObject *obj)
{
  LogflWindow *self = LOGFL_WINDOW (obj);
  g_clear_handle_id (&self->clock_id, g_source_remove);
  g_clear_handle_id (&self->search_id, g_source_remove);
  g_clear_handle_id (&self->tci_retry_id, g_source_remove);
  /* Sentinel for in-flight TCI idles / connect jobs: null the status label
   * first so they drop work instead of touching a half-torn window. */
  self->tci_label = NULL;
  self->wsjtx_label = NULL;
  if (self->wsjtx)
    {
      logfl_wsjtx_server_set_logged_cb (self->wsjtx, NULL, NULL);
      logfl_wsjtx_server_set_status_cb (self->wsjtx, NULL, NULL);
      logfl_wsjtx_server_free (self->wsjtx);
      self->wsjtx = NULL;
    }
  if (self->tci)
    {
      logfl_tci_client_set_state_cb (self->tci, NULL, NULL);
      logfl_tci_client_set_closed_cb (self->tci, NULL, NULL);
      logfl_tci_client_free (self->tci);
      self->tci = NULL;
    }
  g_clear_pointer (&self->pending, logfl_qso_free);
  g_clear_pointer (&self->store_open_error, g_free);
  self->search = NULL;
  self->table_view = NULL;
  self->cell_edit_box = NULL;
  self->exch_box = NULL;
  self->serial_value = NULL;
  self->contest_btn = NULL;
  self->col_stx = NULL;
  self->col_exch = NULL;
  self->col_name = NULL;
  self->col_comment = NULL;
  g_clear_pointer (&self->contest, logfl_contest_free);
  g_clear_pointer (&self->exch_def, logfl_exch_def_free);
  g_clear_pointer (&self->exch_entries, g_ptr_array_unref);
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
  { .name = "about", .activate = act_about },
  { .name = "contest-switch", .parameter_type = "x",
    .activate = act_contest_switch },
  { .name = "contest-new", .activate = act_contest_new },
  { .name = "contest-manage", .activate = act_contest_manage },
};

/* Resolve LogflQsoRow under view coordinates. Only the cell boxes carry
 * "logfl-row", but the hit may land on cell padding (→ the cell widget) or
 * on the row widget itself — so besides walking up, peek one/two levels
 * down into children. At the listview level the row data sits too deep to
 * reach, so a hit between rows still resolves to nothing. */
static LogflQsoRow *
row_from_pick (GtkWidget *root, double x, double y)
{
  GtkWidget *w = gtk_widget_pick (root, x, y, GTK_PICK_DEFAULT);
  for (; w != NULL && w != root; w = gtk_widget_get_parent (w))
    {
      LogflQsoRow *row = g_object_get_data (G_OBJECT (w), "logfl-row");
      if (row)
        return row;
      for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL;
           c = gtk_widget_get_next_sibling (c))
        {
          row = g_object_get_data (G_OBJECT (c), "logfl-row");
          if (!row)
            {
              GtkWidget *b = gtk_widget_get_first_child (c);
              row = b ? g_object_get_data (G_OBJECT (b), "logfl-row") : NULL;
            }
          if (row)
            return row;
        }
    }
  return NULL;
}

/* Right-click on a table row → straight to the delete confirm dialog. (A
 * popover context menu parented to the column view never delivered its
 * action; a single-item menu was one click of indirection anyway — the
 * dialog itself names the QSO and defaults to Cancel.) */
static void
on_table_right_click (GtkGestureClick *gesture, gint n_press,
                      gdouble x, gdouble y, gpointer user_data)
{
  if (n_press != 1)
    return;

  LogflWindow *self = user_data;
  GtkWidget *view =
      gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  LogflQsoRow *row = row_from_pick (view, x, y);
  if (!row)
    return;

  const LogflQso *q = logfl_qso_row_qso (row);
  self->context_qso_id = q && q->id > 0 ? q->id : 0;
  if (self->context_qso_id <= 0)
    return;

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  confirm_delete_context_qso (self);
}

/* QSO table + search bar — sits under the macro strip. */
static GtkWidget *
build_qso_table (LogflWindow *self)
{
  self->rows = g_list_store_new (LOGFL_TYPE_QSO_ROW);
  /* No selection highlight — single click must not flash or fight edit. */
  self->selection = GTK_SELECTION_MODEL (
      gtk_no_selection_new (G_LIST_MODEL (g_object_ref (self->rows))));

  ensure_table_css ();

  /* gtk_column_view_new is transfer-full on the model — hand it its own
   * ref so the one in self->selection stays ours to drop in dispose. */
  GtkWidget *view = gtk_column_view_new (g_object_ref (self->selection));
  self->table_view = view;
  gtk_widget_add_css_class (view, "data-table");
  gtk_column_view_set_single_click_activate (GTK_COLUMN_VIEW (view), FALSE);
  gtk_column_view_set_show_row_separators (GTK_COLUMN_VIEW (view), TRUE);
  gtk_widget_set_tooltip_text (
      view,
      "Click a cell to edit. Right-click a row to delete.");
  /* Preferred widths keep short fields readable; Name/Comment expand.
   * Column order mirrors the entry row (Band/Mode/MHz first), with UTC
   * leading as the timeline. */
  add_column (GTK_COLUMN_VIEW (view), "UTC", COL_UTC, 128, FALSE, self);
  add_column (GTK_COLUMN_VIEW (view), "Band", COL_BAND, 64, FALSE, self);
  add_column (GTK_COLUMN_VIEW (view), "Mode", COL_MODE, 88, FALSE, self);
  add_column (GTK_COLUMN_VIEW (view), "MHz", COL_FREQ, 108, FALSE, self);
  add_column (GTK_COLUMN_VIEW (view), "Call", COL_CALL, 100, FALSE, self);
  add_column (GTK_COLUMN_VIEW (view), "RST s", COL_RST_S, 64, FALSE, self);
  add_column (GTK_COLUMN_VIEW (view), "RST r", COL_RST_R, 64, FALSE, self);
  /* Contest columns — shown only while switched into a contest. */
  self->col_stx = add_column (GTK_COLUMN_VIEW (view), "Sent", COL_STX,
                              72, FALSE, self);
  self->col_exch = add_column (GTK_COLUMN_VIEW (view), "Rcvd", COL_EXCH,
                               96, FALSE, self);
  gtk_column_view_column_set_visible (self->col_stx, FALSE);
  gtk_column_view_column_set_visible (self->col_exch, FALSE);
  self->col_name = add_column (GTK_COLUMN_VIEW (view), "Name", COL_NAME,
                               120, TRUE, self);
  self->col_comment = add_column (GTK_COLUMN_VIEW (view), "Comment",
                                  COL_COMMENT, 160, TRUE, self);

  GtkGesture *rb = GTK_GESTURE (gtk_gesture_click_new ());
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rb), GDK_BUTTON_SECONDARY);
  g_signal_connect (rb, "pressed", G_CALLBACK (on_table_right_click), self);
  gtk_widget_add_controller (view, GTK_EVENT_CONTROLLER (rb));

  GtkWidget *scroller = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroller, TRUE);
  gtk_widget_set_hexpand (scroller, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), view);
  gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroller),
                                              280);

  self->search = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (self->search),
                                         "Search call, name, QTH…");
  gtk_widget_set_hexpand (self->search, TRUE);
  g_signal_connect_swapped (self->search, "search-changed",
                            G_CALLBACK (on_search_changed), self);

  GtkWidget *tools = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (tools), self->search);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_vexpand (box, TRUE);
  gtk_box_append (GTK_BOX (box), tools);
  gtk_box_append (GTK_BOX (box), scroller);
  return box;
}

static void
logfl_window_init (LogflWindow *self)
{
  gtk_window_set_title (GTK_WINDOW (self), "Log for Linux");
  /* Entry + macros + QSO table — wide enough for fixed column prefs. */
  gtk_window_set_default_size (GTK_WINDOW (self), 1200, 760);
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

  /* M9: restore the active contest; a stale id (deleted elsewhere) falls
   * back to the main log. */
  self->exch_entries = g_ptr_array_new ();
  contest_load (self, self->settings.active_contest);
  if (self->settings.active_contest && !self->contest)
    {
      self->settings.active_contest = 0;
      logfl_settings_save (&self->settings);
    }

  /* Header: title · Run/S&P icon · hamburger (pack_end is right-to-left). */
  GtkWidget *header = adw_header_bar_new ();
  self->title = ADW_WINDOW_TITLE (adw_window_title_new ("Log for Linux",
                                                        NULL));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   GTK_WIDGET (self->title));

  GMenu *menu = g_menu_new ();
  g_menu_append (menu, "_Import ADIF…", "win.import");
  g_menu_append (menu, "_Export ADIF…", "win.export");
  g_menu_append (menu, "_Preferences", "win.preferences");
  g_menu_append (menu, "_About Log for Linux", "win.about");
  GtkWidget *menu_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn),
                                 "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_btn),
                                  G_MENU_MODEL (menu));
  g_object_unref (menu);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_btn);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header),
                           build_bank_header_btn (self));

  /* M9: contest switcher lives left of the title. */
  self->contest_btn = gtk_menu_button_new ();
  gtk_menu_button_set_label (GTK_MENU_BUTTON (self->contest_btn), "Main log");
  gtk_widget_set_tooltip_text (self->contest_btn,
                               "Switch between the main log and contests");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), self->contest_btn);

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
  self->freq = mk_entry (self, 10, "7.030000");
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

  /* M9: template exchange fields (rebuilt on contest switch). */
  self->exch_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_visible (self->exch_box, FALSE);

  /* Band/Mode/MHz lead in every view (2026-07-27, Richard) — the radio
   * state first, then the QSO being copied. */
  GtkWidget *fields = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (fields), labeled ("Band", self->band_dd));
  gtk_box_append (GTK_BOX (fields), labeled ("Mode", self->mode_dd));
  gtk_box_append (GTK_BOX (fields), labeled ("MHz", self->freq));
  gtk_box_append (GTK_BOX (fields), labeled ("Call", self->call));
  gtk_box_append (GTK_BOX (fields), labeled ("RST s", self->rst_s));
  gtk_box_append (GTK_BOX (fields), labeled ("RST r", self->rst_r));
  gtk_box_append (GTK_BOX (fields), self->exch_box);
  gtk_box_append (GTK_BOX (fields), labeled ("Name", self->name));
  gtk_box_append (GTK_BOX (fields), labeled ("Comment", self->comment));
  gtk_box_append (GTK_BOX (fields), self->log_btn);

  /* Worked-B4 stays with the entry row (depends on call/band/mode). */
  self->wb4_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->wb4_label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (self->wb4_label),
                           PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (self->wb4_label, TRUE);
  gtk_widget_add_css_class (self->wb4_label, "dim-label");

  GtkWidget *macro_bar = build_macro_bar (self);
  GtkWidget *qso_table = build_qso_table (self);

  GtkWidget *body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (body, 10);
  gtk_widget_set_margin_bottom (body, 6);
  gtk_widget_set_margin_start (body, 12);
  gtk_widget_set_margin_end (body, 12);
  gtk_box_append (GTK_BOX (body), fields);
  gtk_box_append (GTK_BOX (body), macro_bar);
  gtk_box_append (GTK_BOX (body), self->wb4_label);
  gtk_box_append (GTK_BOX (body), qso_table);

  /* F1–F8 / Esc (STOP) — contest-logger style, even while focus is in call. */
  GtkEventController *keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_main_key), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);

  /* Press anywhere outside an open cell editor discards that edit. */
  GtkGesture *outside = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (outside), 0);
  gtk_event_controller_set_propagation_phase (
      GTK_EVENT_CONTROLLER (outside), GTK_PHASE_CAPTURE);
  g_signal_connect (outside, "pressed", G_CALLBACK (on_window_press), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (outside));

  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toasts, body);

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
  self->wsjtx_label = gtk_label_new ("WSJT-X off");
  gtk_widget_add_css_class (self->wsjtx_label, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (self->wsjtx_label), 1);
  gtk_label_set_ellipsize (GTK_LABEL (self->wsjtx_label), PANGO_ELLIPSIZE_END);

  GtkWidget *footer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_set_margin_top (footer, 4);
  gtk_widget_set_margin_bottom (footer, 4);
  gtk_widget_set_margin_start (footer, 12);
  gtk_widget_set_margin_end (footer, 12);
  gtk_box_append (GTK_BOX (footer), self->clock_label);
  gtk_box_append (GTK_BOX (footer), self->tci_label);
  gtk_box_append (GTK_BOX (footer), self->wsjtx_label);

  GtkWidget *tbv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tbv), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tbv),
                                GTK_WIDGET (self->toasts));
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (tbv), footer);
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), tbv);

  self->clock_id = g_timeout_add_seconds (1, clock_tick, self);
  clock_tick (self);
  contest_ui_refresh (self);
  reload (self);
  /* M4: connect to sdr-for-linux TCI in a background thread (non-blocking). */
  g_idle_add (tci_connect_kick, self);
  wsjtx_start (self);
  gtk_widget_grab_focus (self->call);
}

GtkWidget *
logfl_window_new (AdwApplication *app)
{
  return g_object_new (LOGFL_TYPE_WINDOW, "application", app, NULL);
}
