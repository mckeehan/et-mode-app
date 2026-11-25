/*
 * et-mode-app
 * Graphical front-end for selecting the EmComm Tools mode of operation.
 *
 * The list of modes and the logic for starting each mode live in the
 * command line tool `et-mode`. This application:
 *
 *   1. Lists the modes reported by `et-mode --list`.
 *   2. Starts the selected mode with `et-mode <mode-id>` inside an embedded
 *      terminal so that any follow-up prompts (e.g. modem selection) and
 *      log output are visible and interactive.
 *   3. Lets the user cancel the active mode, which stops the terminal's
 *      process group and runs `et-mode none` to stop all services.
 *
 * Created: 2026-11-25
 * Copyright (c) 2026 The Tech Prepper LLC
 */

#include <gtk/gtk.h>
#include <vte/vte.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#define MODE_ID_NONE "none"

typedef enum {
    STATE_IDLE,     /* Showing the list of modes */
    STATE_RUNNING,  /* A mode was started (its command may or may not have exited) */
    STATE_STOPPING  /* `et-mode none` is running */
} AppState;

typedef struct {
    GtkWidget *window;
    GtkWidget *stack;

    /* List page */
    GtkWidget *list_box;
    GtkWidget *last_mode_label;
    GtkWidget *list_error_label;
    GtkWidget *start_button;

    /* Running page */
    GtkWidget *mode_title_label;
    GtkWidget *status_label;
    GtkWidget *cancel_button;
    GtkWidget *terminal;

    AppState state;
    GPid child_pid;           /* -1 when no child is running */
    gchar *mode_id;
    gchar *mode_description;
    gboolean quit_after_stop;
} App;

static const gchar *et_mode_command(void)
{
    /* Allow overriding for development/testing. */
    const gchar *cmd = g_getenv("ET_MODE_CMD");
    return (cmd != NULL && *cmd != '\0') ? cmd : "et-mode";
}

static gchar *read_last_mode(void)
{
    gchar *path;
    gchar *contents = NULL;

    path = g_build_filename(
        g_get_home_dir(),
        ".config",
        "emcomm-tools",
        "et-mode",
        NULL);

    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        g_free(path);
        return NULL;
    }

    g_free(path);
    return g_strstrip(contents);
}

static void set_status(App *app, const gchar *markup)
{
    gtk_label_set_markup(GTK_LABEL(app->status_label), markup);
}

static void feed_message(App *app, const gchar *message)
{
    gchar *line = g_strdup_printf("\r\n\033[1m*** %s ***\033[0m\r\n", message);
    vte_terminal_feed(VTE_TERMINAL(app->terminal), line, -1);
    g_free(line);
}

/* ------------------------------------------------------------------------ */
/* Mode list                                                                */
/* ------------------------------------------------------------------------ */

static void clear_list(App *app)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->list_box));

    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }

    g_list_free(children);
}

static void add_mode_row(App *app,
                         const gchar *mode_id,
                         const gchar *description,
                         gboolean is_last)
{
    GtkWidget *row;
    GtkWidget *box;
    GtkWidget *desc_label;
    GtkWidget *id_label;
    gchar *markup;

    row = gtk_list_box_row_new();
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);

    desc_label = gtk_label_new(description);
    gtk_widget_set_halign(desc_label, GTK_ALIGN_START);

    markup = g_markup_printf_escaped(
        "<span alpha=\"60%%\">%s%s</span>",
        mode_id,
        is_last ? " (last selected)" : "");
    id_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(id_label), markup);
    gtk_widget_set_halign(id_label, GTK_ALIGN_END);
    g_free(markup);

    gtk_box_pack_start(GTK_BOX(box), desc_label, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(box), id_label, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row), box);

    g_object_set_data_full(G_OBJECT(row), "mode-id", g_strdup(mode_id), g_free);
    g_object_set_data_full(G_OBJECT(row), "mode-description",
                           g_strdup(description), g_free);

    gtk_container_add(GTK_CONTAINER(app->list_box), row);

    if (is_last) {
        gtk_list_box_select_row(GTK_LIST_BOX(app->list_box),
                                GTK_LIST_BOX_ROW(row));
    }
}

static void load_modes(App *app)
{
    gchar *argv[] = { (gchar *)et_mode_command(), "--list", NULL };
    gchar *out = NULL;
    gchar *err = NULL;
    gint wait_status = 0;
    GError *error = NULL;
    gchar *last_mode;
    gchar **lines;
    gint count = 0;

    clear_list(app);
    gtk_widget_hide(app->list_error_label);

    last_mode = read_last_mode();

    if (last_mode != NULL && *last_mode != '\0' &&
        g_strcmp0(last_mode, MODE_ID_NONE) != 0) {
        gchar *markup = g_markup_printf_escaped(
            "Last selected mode: <b>%s</b>", last_mode);
        gtk_label_set_markup(GTK_LABEL(app->last_mode_label), markup);
        gtk_widget_show(app->last_mode_label);
        g_free(markup);
    } else {
        gtk_widget_hide(app->last_mode_label);
    }

    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &out, &err, &wait_status, &error) ||
        !g_spawn_check_wait_status(wait_status, NULL)) {
        gchar *markup = g_markup_printf_escaped(
            "<span foreground=\"red\">Unable to list modes with '%s --list'%s%s</span>",
            argv[0],
            error != NULL ? ": " : ".",
            error != NULL ? error->message : "");
        gtk_label_set_markup(GTK_LABEL(app->list_error_label), markup);
        gtk_widget_show(app->list_error_label);
        g_free(markup);
        g_clear_error(&error);
        g_free(out);
        g_free(err);
        g_free(last_mode);
        return;
    }

    lines = g_strsplit(out, "\n", -1);

    for (gchar **line = lines; *line != NULL; line++) {
        gchar **fields;

        if (**line == '\0') {
            continue;
        }

        fields = g_strsplit(*line, "\t", 2);

        /* "none" is what Cancel Mode does, so it isn't offered as a mode. */
        if (fields[0] != NULL && fields[1] != NULL &&
            g_strcmp0(fields[0], MODE_ID_NONE) != 0) {
            add_mode_row(app, fields[0], fields[1],
                         g_strcmp0(fields[0], last_mode) == 0);
            count++;
        }

        g_strfreev(fields);
    }

    g_strfreev(lines);
    g_free(out);
    g_free(err);
    g_free(last_mode);

    if (count == 0) {
        gtk_label_set_markup(
            GTK_LABEL(app->list_error_label),
            "<span foreground=\"red\">No modes are available.</span>");
        gtk_widget_show(app->list_error_label);
    }

    gtk_widget_show_all(app->list_box);
}

/* ------------------------------------------------------------------------ */
/* Terminal / process handling                                              */
/* ------------------------------------------------------------------------ */

static void stop_child(App *app)
{
    if (app->child_pid > 0) {
        /* VTE makes the child a session leader, so its pid is also the
         * process group id. Signal the whole group so that foreground
         * applications started by et-mode (e.g. YAAC) are stopped too. */
        kill(-app->child_pid, SIGTERM);
        app->child_pid = -1;
    }
}

static void spawn_ready(VteTerminal *terminal,
                        GPid pid,
                        GError *error,
                        gpointer user_data)
{
    App *app = user_data;

    (void)terminal;

    if (error != NULL) {
        gchar *msg = g_strdup_printf("Unable to start command: %s",
                                     error->message);
        feed_message(app, msg);
        g_free(msg);

        app->child_pid = -1;

        if (app->state == STATE_RUNNING) {
            set_status(app, "<span foreground=\"red\">Failed to start</span>");
        } else if (app->state == STATE_STOPPING) {
            /* Nothing more we can do; return to the list. */
            app->state = STATE_IDLE;
            if (app->quit_after_stop) {
                gtk_widget_destroy(app->window);
                return;
            }
            load_modes(app);
            gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "list");
        }
        return;
    }

    app->child_pid = pid;

    if (app->state == STATE_RUNNING) {
        set_status(app, "<span foreground=\"green\">Running</span>");
    }
}

static void spawn_in_terminal(App *app, const gchar *mode_id)
{
    gchar *argv[] = { (gchar *)et_mode_command(), (gchar *)mode_id, NULL };

    vte_terminal_spawn_async(
        VTE_TERMINAL(app->terminal),
        VTE_PTY_DEFAULT,
        g_get_home_dir(),
        argv,
        NULL,
        G_SPAWN_SEARCH_PATH,
        NULL, NULL, NULL,
        -1,
        NULL,
        spawn_ready,
        app);
}

static void child_exited(VteTerminal *terminal, gint status, gpointer user_data)
{
    App *app = user_data;

    (void)terminal;

    switch (app->state) {
    case STATE_RUNNING:
        app->child_pid = -1;

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            feed_message(app, "Mode started. Use 'Cancel Mode' to stop it.");
            set_status(app, "<span foreground=\"green\">Active (services running in the background)</span>");
        } else {
            gchar *markup;
            gint code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

            feed_message(app, "Mode command exited with an error.");
            markup = g_strdup_printf(
                "<span foreground=\"red\">Exited with an error (%d)</span>", code);
            set_status(app, markup);
            g_free(markup);
        }
        break;

    case STATE_STOPPING:
        if (app->child_pid <= 0) {
            /* The cancelled mode process exited before `et-mode none`
             * finished spawning; wait for the stop command instead. */
            break;
        }

        app->child_pid = -1;
        app->state = STATE_IDLE;

        if (app->quit_after_stop) {
            gtk_widget_destroy(app->window);
            return;
        }

        load_modes(app);
        gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "list");
        gtk_widget_grab_focus(app->list_box);
        break;

    case STATE_IDLE:
    default:
        break;
    }
}

static void start_mode(App *app, const gchar *mode_id, const gchar *description)
{
    gchar *markup;

    g_free(app->mode_id);
    g_free(app->mode_description);
    app->mode_id = g_strdup(mode_id);
    app->mode_description = g_strdup(description);

    markup = g_markup_printf_escaped(
        "<span size=\"large\"><b>%s</b></span>  <span alpha=\"60%%\">%s</span>",
        description, mode_id);
    gtk_label_set_markup(GTK_LABEL(app->mode_title_label), markup);
    g_free(markup);

    set_status(app, "Starting…");
    gtk_widget_set_sensitive(app->cancel_button, TRUE);

    vte_terminal_reset(VTE_TERMINAL(app->terminal), TRUE, TRUE);

    app->state = STATE_RUNNING;
    gtk_stack_set_visible_child_name(GTK_STACK(app->stack), "running");

    spawn_in_terminal(app, mode_id);
    gtk_widget_grab_focus(app->terminal);
}

static void stop_mode(App *app)
{
    app->state = STATE_STOPPING;
    set_status(app, "Stopping…");
    gtk_widget_set_sensitive(app->cancel_button, FALSE);

    stop_child(app);
    spawn_in_terminal(app, MODE_ID_NONE);
}

/* ------------------------------------------------------------------------ */
/* Signal handlers                                                          */
/* ------------------------------------------------------------------------ */

static void start_selected(App *app)
{
    GtkListBoxRow *row;

    row = gtk_list_box_get_selected_row(GTK_LIST_BOX(app->list_box));

    if (row == NULL) {
        return;
    }

    start_mode(app,
               g_object_get_data(G_OBJECT(row), "mode-id"),
               g_object_get_data(G_OBJECT(row), "mode-description"));
}

static void start_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    start_selected(user_data);
}

static void row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    (void)row;
    (void)box;
    start_selected(user_data);
}

static void row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    App *app = user_data;

    (void)box;
    gtk_widget_set_sensitive(app->start_button, row != NULL);
}

static void refresh_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    load_modes(user_data);
}

static void cancel_clicked(GtkButton *button, gpointer user_data)
{
    App *app = user_data;
    GtkWidget *dialog;
    gint response;

    (void)button;

    dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE,
        "Cancel %s?",
        app->mode_description);
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(dialog),
        "All EmComm Tools modems and radio applications will be stopped.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog),
                           "Keep Running", GTK_RESPONSE_CANCEL,
                           "Cancel Mode", GTK_RESPONSE_ACCEPT,
                           NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);

    response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_ACCEPT) {
        stop_mode(app);
    }
}

#define RESPONSE_LEAVE_RUNNING 1
#define RESPONSE_STOP_AND_QUIT 2

static gboolean window_delete(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    App *app = user_data;
    GtkWidget *dialog;
    gint response;

    (void)widget;
    (void)event;

    if (app->state == STATE_IDLE) {
        return FALSE;
    }

    if (app->state == STATE_STOPPING) {
        /* Let `et-mode none` finish, then quit. */
        app->quit_after_stop = TRUE;
        return TRUE;
    }

    dialog = gtk_message_dialog_new(
        GTK_WINDOW(app->window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE,
        "%s is active.",
        app->mode_description);

    gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", GTK_RESPONSE_CANCEL);

    if (app->child_pid > 0) {
        /* Closing the terminal would kill the foreground application. */
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialog),
            "Quitting will stop the mode and all EmComm Tools services.");
    } else {
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dialog),
            "You can leave the mode's services running in the background "
            "or stop them before quitting.");
        gtk_dialog_add_button(GTK_DIALOG(dialog), "Leave Running",
                              RESPONSE_LEAVE_RUNNING);
    }

    gtk_dialog_add_button(GTK_DIALOG(dialog), "Stop Mode and Quit",
                          RESPONSE_STOP_AND_QUIT);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);

    response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    switch (response) {
    case RESPONSE_LEAVE_RUNNING:
        return FALSE;
    case RESPONSE_STOP_AND_QUIT:
        app->quit_after_stop = TRUE;
        stop_mode(app);
        return TRUE;
    default:
        return TRUE;
    }
}

/* ------------------------------------------------------------------------ */
/* UI construction                                                          */
/* ------------------------------------------------------------------------ */

static GtkWidget *build_list_page(App *app)
{
    GtkWidget *page;
    GtkWidget *title;
    GtkWidget *scrolled;
    GtkWidget *button_box;
    GtkWidget *refresh_button;
    GtkWidget *quit_button;

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 16);

    title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
                         "<span size=\"large\"><b>Select a mode</b></span>");
    gtk_widget_set_halign(title, GTK_ALIGN_START);

    app->last_mode_label = gtk_label_new(NULL);
    gtk_widget_set_halign(app->last_mode_label, GTK_ALIGN_START);
    gtk_widget_set_no_show_all(app->last_mode_label, TRUE);

    app->list_error_label = gtk_label_new(NULL);
    gtk_widget_set_halign(app->list_error_label, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(app->list_error_label), TRUE);
    gtk_widget_set_no_show_all(app->list_error_label, TRUE);

    app->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->list_box),
                                    GTK_SELECTION_BROWSE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(app->list_box), FALSE);
    g_signal_connect(app->list_box, "row-activated",
                     G_CALLBACK(row_activated), app);
    g_signal_connect(app->list_box, "row-selected",
                     G_CALLBACK(row_selected), app);

    scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled),
                                        GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scrolled), app->list_box);

    button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    refresh_button = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_button, "clicked",
                     G_CALLBACK(refresh_clicked), app);

    quit_button = gtk_button_new_with_label("Quit");
    g_signal_connect_swapped(quit_button, "clicked",
                             G_CALLBACK(gtk_widget_destroy), app->window);

    app->start_button = gtk_button_new_with_label("Start Mode");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->start_button),
                                "suggested-action");
    gtk_widget_set_sensitive(app->start_button, FALSE);
    gtk_widget_set_can_default(app->start_button, TRUE);
    gtk_window_set_default(GTK_WINDOW(app->window), app->start_button);
    g_signal_connect(app->start_button, "clicked",
                     G_CALLBACK(start_clicked), app);

    gtk_box_pack_start(GTK_BOX(button_box), refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(button_box), app->start_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(button_box), quit_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), app->last_mode_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), app->list_error_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), scrolled, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), button_box, FALSE, FALSE, 0);

    return page;
}

static GtkWidget *build_running_page(App *app)
{
    GtkWidget *page;
    GtkWidget *header;
    GtkWidget *labels;
    GtkWidget *term_box;
    GtkWidget *scrollbar;
    GtkWidget *frame;

    page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 16);

    header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    app->mode_title_label = gtk_label_new(NULL);
    gtk_widget_set_halign(app->mode_title_label, GTK_ALIGN_START);

    app->status_label = gtk_label_new(NULL);
    gtk_widget_set_halign(app->status_label, GTK_ALIGN_START);

    gtk_box_pack_start(GTK_BOX(labels), app->mode_title_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(labels), app->status_label, FALSE, FALSE, 0);

    app->cancel_button = gtk_button_new_with_label("Cancel Mode");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->cancel_button),
                                "destructive-action");
    gtk_widget_set_valign(app->cancel_button, GTK_ALIGN_CENTER);
    g_signal_connect(app->cancel_button, "clicked",
                     G_CALLBACK(cancel_clicked), app);

    gtk_box_pack_start(GTK_BOX(header), labels, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(header), app->cancel_button, FALSE, FALSE, 0);

    app->terminal = vte_terminal_new();
    vte_terminal_set_scrollback_lines(VTE_TERMINAL(app->terminal), 10000);
    vte_terminal_set_scroll_on_output(VTE_TERMINAL(app->terminal), TRUE);
    vte_terminal_set_scroll_on_keystroke(VTE_TERMINAL(app->terminal), TRUE);
    g_signal_connect(app->terminal, "child-exited",
                     G_CALLBACK(child_exited), app);

    scrollbar = gtk_scrollbar_new(
        GTK_ORIENTATION_VERTICAL,
        gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(app->terminal)));

    term_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(term_box), app->terminal, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(term_box), scrollbar, FALSE, FALSE, 0);

    frame = gtk_frame_new(NULL);
    gtk_container_add(GTK_CONTAINER(frame), term_box);

    gtk_box_pack_start(GTK_BOX(page), header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), frame, TRUE, TRUE, 0);

    return page;
}

int main(int argc, char *argv[])
{
    App app = { 0 };
    GdkDisplay *display;
    GdkMonitor *monitor;
    GdkRectangle geometry;
    int width;
    int height;

    gtk_init(&argc, &argv);

    app.state = STATE_IDLE;
    app.child_pid = -1;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app.window), "EmComm Tools Mode");

    display = gdk_display_get_default();
    monitor = gdk_display_get_primary_monitor(display);
    if (monitor == NULL) {
        monitor = gdk_display_get_monitor(display, 0);
    }
    gdk_monitor_get_geometry(monitor, &geometry);

    width = CLAMP((int)(geometry.width * 0.55), 640, 1000);
    height = CLAMP((int)(geometry.height * 0.6), 480, 800);

    gtk_window_set_default_size(GTK_WINDOW(app.window), width, height);
    // Wayland does support centering.
    // Force x11 as the backend: GDK_BACKEND=x11 et-mode-app
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);

    app.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app.stack),
                                  GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(GTK_STACK(app.stack), build_list_page(&app), "list");
    gtk_stack_add_named(GTK_STACK(app.stack), build_running_page(&app), "running");
    gtk_container_add(GTK_CONTAINER(app.window), app.stack);

    g_signal_connect(app.window, "delete-event", G_CALLBACK(window_delete), &app);
    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    gtk_widget_show_all(app.window);
    gtk_stack_set_visible_child_name(GTK_STACK(app.stack), "list");

    load_modes(&app);

    gtk_main();

    g_free(app.mode_id);
    g_free(app.mode_description);

    return 0;
}
