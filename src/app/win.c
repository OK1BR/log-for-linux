/* win.c — the main logbook window: entry row (UTC clock, worked-B4 hint),
 * QSO table, search, ADIF import/export, delete with confirmation (M3).
 *
 * Part of log-for-linux. GPL-3.0-or-later.
 */
#include "win.h"

#include <string.h>

#include "adif.h"
#include "engine.h"
#include "log_store.h"
#include "qso_row.h"

#define DUP_WINDOW_S 300

static const char *bands[] = { "160m", "80m", "60m", "40m", "30m", "20m",
                               "17m", "15m", "12m", "10m", "6m", "2m",
                               "70cm", NULL };
static const char *modes[] = { "CW", "SSB", "FT8", "FT4", "RTTY", "PSK31",
                               "FM", "AM", NULL };

enum { COL_UTC, COL_CALL, COL_BAND, COL_FREQ, COL_MODE, COL_RST,
       COL_NAME, COL_COMMENT };

struct _LogflWindow {
  AdwApplicationWindow parent_instance;

  LogflStore *store;
  char *db_path;

  GListStore *rows;
  GtkSingleSelection *selection;

  AdwWindowTitle *title;
  AdwToastOverlay *toasts;
  GtkWidget *search;
  GtkWidget *call, *rst_s, *rst_r, *freq, *name, *comment;
  GtkWidget *band_dd, *mode_dd;
  GtkWidget *wb4_label, *clock_label;
  GtkWidget *delete_btn;

  guint clock_id;
  gboolean syncing_freq;       /* guard against freq↔band feedback */
  LogflQso *pending;           /* QSO awaiting dup confirmation */
  gint64 pending_delete_id;
};

G_DEFINE_FINAL_TYPE (LogflWindow, logfl_window, ADW_TYPE_APPLICATION_WINDOW)

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
  GError *err = NULL;
  const char *text = entry_text (self->search);
  LogflStoreQuery q = { .text = *text ? text : NULL };
  GPtrArray *list = logfl_store_list (self->store, &q, &err);

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
      g_free (sub);
    }
}

/* --- entry row logic ---------------------------------------------------- */

static void
update_wb4 (LogflWindow *self)
{
  const char *call = entry_text (self->call);
  GtkWidget *l = self->wb4_label;

  gtk_widget_remove_css_class (l, "success");
  gtk_widget_remove_css_class (l, "warning");
  if (strlen (call) < 2)
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

static void
on_mode_changed (LogflWindow *self)
{
  /* Refresh the RST defaults, but never stomp a hand-edited report. */
  const char *mode = dd_selected (self->mode_dd, modes);
  const char *def =
      (g_strcmp0 (mode, "SSB") == 0 || g_strcmp0 (mode, "FM") == 0 ||
       g_strcmp0 (mode, "AM") == 0) ? "59" : "599";
  const char *cur_s = entry_text (self->rst_s);
  const char *cur_r = entry_text (self->rst_r);
  if (!*cur_s || g_str_equal (cur_s, "59") || g_str_equal (cur_s, "599"))
    gtk_editable_set_text (GTK_EDITABLE (self->rst_s), def);
  if (!*cur_r || g_str_equal (cur_r, "59") || g_str_equal (cur_r, "599"))
    gtk_editable_set_text (GTK_EDITABLE (self->rst_r), def);
  update_wb4 (self);
}

static void
on_freq_changed (LogflWindow *self)
{
  if (self->syncing_freq)
    return;
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
do_add_pending (LogflWindow *self)
{
  GError *err = NULL;
  LogflQso *q = g_steal_pointer (&self->pending);
  if (logfl_store_add (self->store, q, &err))
    {
      toast (self, "Logged %s · %s · %s", q->call, q->band, q->mode);
      clear_entry_row (self);
      reload (self);
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
  const char *call = entry_text (self->call);
  if (!*call)
    {
      toast (self, "Callsign first");
      gtk_widget_grab_focus (self->call);
      return;
    }

  LogflQso *q = logfl_qso_new ();
  q->call = g_strdup (call);
  q->band = g_strdup (dd_selected (self->band_dd, bands));
  q->mode = g_strdup (dd_selected (self->mode_dd, modes));
  q->ts = g_get_real_time () / G_USEC_PER_SEC;
  char *ftxt = g_strdup (entry_text (self->freq));
  g_strdelimit (ftxt, ",", '.');
  q->freq = g_ascii_strtod (ftxt, NULL);
  g_free (ftxt);
  q->rst_sent = g_strdup (entry_text (self->rst_s));
  q->rst_rcvd = g_strdup (entry_text (self->rst_r));
  q->name = g_strdup (entry_text (self->name));
  q->comment = g_strdup (entry_text (self->comment));
  q->station_callsign = g_strdup ("OK1BR");

  gboolean dup = FALSE;
  logfl_store_dup_check (self->store, q->call, q->band, q->mode, q->ts,
                         DUP_WINDOW_S, &dup, NULL);
  g_clear_pointer (&self->pending, logfl_qso_free);
  self->pending = q;
  if (!dup)
    {
      do_add_pending (self);
      return;
    }

  AdwDialog *dlg = adw_alert_dialog_new ("Duplicate?", NULL);
  adw_alert_dialog_format_body (ADW_ALERT_DIALOG (dlg),
      "%s was already logged on %s/%s within the last %d min.",
      q->call, q->band, q->mode, DUP_WINDOW_S / 60);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dlg),
                                  "cancel", "Cancel", "log", "Log anyway",
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dlg), "log",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dlg), "log");
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dlg), GTK_WIDGET (self), NULL,
                           on_dup_response, self);
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
  if (!g_str_equal (resp, "delete"))
    return;
  GError *err = NULL;
  if (logfl_store_delete (self->store, self->pending_delete_id, &err))
    {
      toast (self, "QSO deleted");
      reload (self);
    }
  else
    {
      toast (self, "Delete failed: %s", err->message);
      g_clear_error (&err);
    }
}

static void
on_delete_clicked (GtkButton *btn, gpointer user_data)
{
  (void) btn;
  LogflWindow *self = user_data;
  LogflQsoRow *row = gtk_single_selection_get_selected_item (self->selection);
  if (!row)
    return;
  const LogflQso *q = logfl_qso_row_qso (row);
  self->pending_delete_id = q->id;

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
  char *path = g_file_get_path (file);
  LogflAdifReport rep;
  if (logfl_adif_import_file (self->store, path, DUP_WINDOW_S, &rep, &err))
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
  g_free (path);
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
  char *path = g_file_get_path (file);
  guint n = 0;
  if (logfl_adif_export_file (self->store, path, NULL, &n, &err))
    toast (self, "Exported %u QSO to %s", n, path);
  else
    {
      toast (self, "Export failed: %s", err->message);
      g_clear_error (&err);
    }
  g_free (path);
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
  char *dbg = g_strdup_printf ("SQLite %s\nLog: %s",
                               logfl_engine_sqlite_version (), self->db_path);
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
  g_signal_connect_swapped (e, "activate", G_CALLBACK (log_qso), self);
  return e;
}

static void
logfl_window_dispose (GObject *obj)
{
  LogflWindow *self = LOGFL_WINDOW (obj);
  g_clear_handle_id (&self->clock_id, g_source_remove);
  g_clear_pointer (&self->pending, logfl_qso_free);
  g_clear_object (&self->rows);
  g_clear_pointer (&self->store, logfl_store_close);
  g_clear_pointer (&self->db_path, g_free);
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
  { .name = "about", .activate = act_about },
};

static void
logfl_window_init (LogflWindow *self)
{
  gtk_window_set_title (GTK_WINDOW (self), "Log for Linux");
  gtk_window_set_default_size (GTK_WINDOW (self), 1150, 700);
  g_action_map_add_action_entries (G_ACTION_MAP (self), win_actions,
                                   G_N_ELEMENTS (win_actions), self);

  /* Store. */
  GError *err = NULL;
  char *dir = g_build_filename (g_get_user_data_dir (), "log-for-linux",
                                NULL);
  g_mkdir_with_parents (dir, 0700);
  self->db_path = g_build_filename (dir, "log.db", NULL);
  g_free (dir);
  self->store = logfl_store_open (self->db_path, &err);
  if (!self->store)
    g_error ("cannot open log store %s: %s", self->db_path, err->message);

  /* Header bar: title + search, menu. */
  GtkWidget *header = adw_header_bar_new ();
  self->title = ADW_WINDOW_TITLE (adw_window_title_new ("Log for Linux",
                                                        NULL));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   GTK_WIDGET (self->title));

  self->search = gtk_search_entry_new ();
  gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (self->search),
                                         "Search call, name, QTH…");
  gtk_widget_set_size_request (self->search, 240, -1);
  g_signal_connect_swapped (self->search, "search-changed",
                            G_CALLBACK (reload), self);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), self->search);

  self->delete_btn = gtk_button_new_from_icon_name ("user-trash-symbolic");
  gtk_widget_set_tooltip_text (self->delete_btn, "Delete selected QSO");
  g_signal_connect (self->delete_btn, "clicked",
                    G_CALLBACK (on_delete_clicked), self);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), self->delete_btn);

  GMenu *menu = g_menu_new ();
  g_menu_append (menu, "_Import ADIF…", "win.import");
  g_menu_append (menu, "_Export ADIF…", "win.export");
  g_menu_append (menu, "_About Log for Linux", "win.about");
  GtkWidget *menu_btn = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn),
                                 "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_btn),
                                  G_MENU_MODEL (menu));
  g_object_unref (menu);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_btn);

  /* Entry row. */
  self->call = mk_entry (self, 10, "OK1…");
  g_signal_connect_swapped (self->call, "changed",
                            G_CALLBACK (update_wb4), self);
  self->rst_s = mk_entry (self, 4, NULL);
  self->rst_r = mk_entry (self, 4, NULL);
  gtk_editable_set_text (GTK_EDITABLE (self->rst_s), "599");
  gtk_editable_set_text (GTK_EDITABLE (self->rst_r), "599");
  self->band_dd = gtk_drop_down_new_from_strings (bands);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (self->band_dd), 3); /* 40m */
  g_signal_connect_swapped (self->band_dd, "notify::selected",
                            G_CALLBACK (update_wb4), self);
  self->mode_dd = gtk_drop_down_new_from_strings (modes);
  g_signal_connect_swapped (self->mode_dd, "notify::selected",
                            G_CALLBACK (on_mode_changed), self);
  self->freq = mk_entry (self, 8, "7.030");
  g_signal_connect_swapped (self->freq, "changed",
                            G_CALLBACK (on_freq_changed), self);
  self->name = mk_entry (self, 12, NULL);
  self->comment = mk_entry (self, 18, NULL);
  gtk_widget_set_hexpand (self->comment, TRUE);

  GtkWidget *log_btn = gtk_button_new_with_label ("Log QSO");
  gtk_widget_add_css_class (log_btn, "suggested-action");
  gtk_widget_set_valign (log_btn, GTK_ALIGN_END);
  g_signal_connect_swapped (log_btn, "clicked", G_CALLBACK (log_qso), self);

  GtkWidget *fields = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (fields), labeled ("Call", self->call));
  gtk_box_append (GTK_BOX (fields), labeled ("RST s", self->rst_s));
  gtk_box_append (GTK_BOX (fields), labeled ("RST r", self->rst_r));
  gtk_box_append (GTK_BOX (fields), labeled ("Band", self->band_dd));
  gtk_box_append (GTK_BOX (fields), labeled ("Mode", self->mode_dd));
  gtk_box_append (GTK_BOX (fields), labeled ("MHz", self->freq));
  gtk_box_append (GTK_BOX (fields), labeled ("Name", self->name));
  gtk_box_append (GTK_BOX (fields), labeled ("Comment", self->comment));
  gtk_box_append (GTK_BOX (fields), log_btn);

  self->clock_label = gtk_label_new ("");
  gtk_widget_add_css_class (self->clock_label, "numeric");
  gtk_widget_add_css_class (self->clock_label, "dim-label");
  gtk_label_set_xalign (GTK_LABEL (self->clock_label), 0);
  self->wb4_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (self->wb4_label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (self->wb4_label),
                           PANGO_ELLIPSIZE_END);
  GtkWidget *info = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 24);
  gtk_box_append (GTK_BOX (info), self->clock_label);
  gtk_box_append (GTK_BOX (info), self->wb4_label);

  GtkWidget *entry_bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top (entry_bar, 10);
  gtk_widget_set_margin_bottom (entry_bar, 10);
  gtk_widget_set_margin_start (entry_bar, 12);
  gtk_widget_set_margin_end (entry_bar, 12);
  gtk_box_append (GTK_BOX (entry_bar), fields);
  gtk_box_append (GTK_BOX (entry_bar), info);

  /* Table. */
  self->rows = g_list_store_new (LOGFL_TYPE_QSO_ROW);
  self->selection =
      gtk_single_selection_new (G_LIST_MODEL (g_object_ref (self->rows)));
  gtk_single_selection_set_autoselect (self->selection, FALSE);
  GtkWidget *view = gtk_column_view_new (GTK_SELECTION_MODEL (self->selection));
  gtk_widget_add_css_class (view, "data-table");
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
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), view);

  /* Assembly. */
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append (GTK_BOX (content), entry_bar);
  gtk_box_append (GTK_BOX (content),
                  gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append (GTK_BOX (content), scroller);

  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toasts, content);

  GtkWidget *tbv = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (tbv), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (tbv),
                                GTK_WIDGET (self->toasts));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), tbv);

  self->clock_id = g_timeout_add_seconds (1, clock_tick, self);
  clock_tick (self);
  reload (self);
  gtk_widget_grab_focus (self->call);
}

GtkWidget *
logfl_window_new (AdwApplication *app)
{
  return g_object_new (LOGFL_TYPE_WINDOW, "application", app, NULL);
}
