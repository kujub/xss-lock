#include <locale.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <xcb/xcb_event.h>
#include <xcb/screensaver.h>

#include "config.h"
#include "x_event_source.h"
#include "logind_manager.h"
#include "logind_session.h"

#define LOGIND_SERVICE "org.freedesktop.login1"
#define LOGIND_PATH    "/org/freedesktop/login1"

#define XCB_SCREENSAVER_PROPERTY_NAME "_MIT_SCREEN_SAVER_ID"
#define XCB_ERROR xcb_error_quark()

GQuark
xcb_error_quark(void) G_GNUC_CONST
{
    return g_quark_from_static_string("xcb-error-quark");
}

typedef struct Child {
    gchar *name;
    gchar **cmd;
    GPid pid;
    struct Child *kill_first;
} Child;

static const xcb_screen_t *get_screen(xcb_connection_t *connection, int screen_number);
static gboolean register_screensaver(xcb_connection_t *connection, xcb_screen_t *screen, xcb_atom_t *atom, GError **error);
static gboolean screensaver_event_cb(xcb_connection_t *connection, xcb_generic_event_t *event, const int *xcb_screensaver_notify);

static void start_child(Child *child);
static void kill_child(Child *child);
static void child_watch_cb(GPid pid, gint status, Child *child);

static void logind_manager_proxy_new_cb(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void take_sleep_delay_lock(void);
static void logind_manager_call_inhibit_cb(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void logind_manager_prepare_for_sleep_cb(gboolean active);
static void logind_manager_call_get_session_cb(GObject *source_object, GAsyncResult *res, gpointer user_data);
static void logind_session_proxy_new_cb(GObject *source_object, GAsyncResult *res, gpointer user_data);

static gboolean parse_options(int argc, char *argv[], GError **error);
static gboolean parse_locker_cmd(const gchar *option_name, const gchar *value, gpointer data, GError **error);
static gboolean parse_notifier_cmd(const gchar *option_name, const gchar *value, gpointer data, GError **error);
static gboolean parse_child_cmd(const gchar *option_name, const gchar *value, Child *child, GError **error);
static gboolean reset_screensaver(xcb_connection_t *connection);
static gboolean exit_service(GMainLoop *loop);

static Child notifier = {"notifier", NULL, 0, NULL};
static Child locker = {"locker", NULL, 0, &notifier};
static gboolean opt_ignore_sleep = FALSE;

static GOptionEntry opt_entries[] = {
    {"locker", 'l', G_OPTION_FLAG_FILENAME, G_OPTION_ARG_CALLBACK, parse_locker_cmd, "lock command", "CMD"},
    {"notifier", 'n', G_OPTION_FLAG_FILENAME, G_OPTION_ARG_CALLBACK, parse_notifier_cmd, "notify command", "CMD"},
    {"ignore-sleep", 0, 0, G_OPTION_ARG_NONE, &opt_ignore_sleep, "do not lock on suspend/hibernate", NULL},
    {NULL}
};

static logindManager *logind_manager = NULL;

static logindSession *logind_session = NULL;

static gint32 sleep_lock_fd = -1;

static const xcb_screen_t *
get_screen(xcb_connection_t *connection, int screen_number)
{
    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);

    g_return_val_if_fail(screen_number < xcb_setup_roots_length(setup), NULL);

    while (screen_number--)
        xcb_screen_next(&iter);

    return iter.data;
}

static gboolean
register_screensaver(xcb_connection_t *connection, xcb_screen_t *screen,
                     xcb_atom_t *atom, GError **error)
{
    xcb_screensaver_query_version_cookie_t version_cookie;
    xcb_screensaver_query_version_reply_t *version_reply;
    xcb_intern_atom_cookie_t atom_cookie;
    xcb_intern_atom_reply_t *atom_reply;
    xcb_void_cookie_t set_attributes_cookie;
    xcb_generic_error_t *xcb_error;

    version_cookie = xcb_screensaver_query_version(connection, 1, 0);
    set_attributes_cookie =
        xcb_screensaver_set_attributes_checked(connection, screen->root,
                                               -1, -1, 1, 1, 0,
                                               XCB_COPY_FROM_PARENT,
                                               XCB_COPY_FROM_PARENT,
                                               XCB_COPY_FROM_PARENT,
                                               0, NULL);
    atom_cookie = xcb_intern_atom(connection, FALSE,
                                  strlen(XCB_SCREENSAVER_PROPERTY_NAME),
                                  XCB_SCREENSAVER_PROPERTY_NAME);

    xcb_screensaver_select_input(connection, screen->root,
                                 XCB_SCREENSAVER_EVENT_NOTIFY_MASK |
                                 XCB_SCREENSAVER_EVENT_CYCLE_MASK);

    if (xcb_error = xcb_request_check(connection, set_attributes_cookie))
        goto xcb_error;

    version_reply = xcb_screensaver_query_version_reply(connection,
                                                        version_cookie,
                                                        &xcb_error);
    atom_reply = xcb_intern_atom_reply(connection, atom_cookie, &xcb_error);
    // TODO: create pixmap, get its xid
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, screen->root,
                        atom_reply->atom, XCB_ATOM_PIXMAP, 32, 1, xid);
    // TODO: register atom, need XID or something?
    return TRUE;
    // TODO: free stuff, rework goto/label
xcb_error:
    g_set_error(error, XCB_ERROR, 0, );
    return FALSE;
}

static gboolean
screensaver_event_cb(xcb_connection_t *connection, xcb_generic_event_t *event,
                     const int *xcb_screensaver_notify)
{
    const uint8_t type = XCB_EVENT_RESPONSE_TYPE(event);

    if (type == 0) {
        xcb_generic_error_t *xcb_error = (xcb_generic_error_t *)event;
        // TODO: print
    } else if (type == *xcb_screensaver_notify) {
        xcb_screensaver_notify_event_t *xss_event =
            (xcb_screensaver_notify_event_t *)event;
        // check for XCB_SCREENSAVER_KIND_EXTERNAL first?
        switch (xss_event->state) {
        case XCB_SCREENSAVER_STATE_ON:
            if (xss_event->kind == XCB_SCREENSAVER_KIND_INTERNAL) {
                // deactivate internal, start external saver (i.e., me)
                // TODO:
                // - make this optional
                // - try to see it in action
                // - figure out if it also generates an OFF event (to be ignored)
                xcb_force_screen_saver(connection, XCB_SCREEN_SAVER_ACTIVE);
            } else if (!notifier.cmd || xss_event->forced) {
                start_child(&locker);
            } else {
                start_child(&notifier);
            }
            break;
        case XCB_SCREENSAVER_STATE_OFF:
            kill_child(&notifier);
            if (xss_event->forced)
                kill_child(&locker);
            break;
        case XCB_SCREENSAVER_STATE_CYCLE:
            start_child(&locker);
            break;
        }
    }
    return TRUE;
}

static void
start_child(Child *child)
{
    const GSpawnFlags flags = G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD;
    GError *error = NULL;

    if (child->pid)
        return;

    if (child->kill_first)
        kill_child(child->kill_first);

    if (!g_spawn_async(NULL, child->cmd, NULL, flags, NULL, NULL,
                       &child->pid, &error)) {
        g_printerr("Error spawning %s: ", child->name, error->message);
        g_error_free(error);
        return;
    }
    g_child_watch_add(child->pid, (GChildWatchFunc)child_watch_cb, child);
}

static void
kill_child(Child *child)
{
    if (child->pid && kill(child->pid, SIGTERM))
        g_printerr("Error sending SIGTERM to %s: %s",
                   child->name, g_strerror(errno));
}

static void
child_watch_cb(GPid pid, gint status, Child *child)
{
    GError *error = NULL;

    if (!g_spawn_check_exit_status(status, &error)) { // TODO: replace by UNIX-specific functions, otherwise this requires glib>=2.34 vs. 2.30 for g_unix_signal_add
        g_printerr("%s exited abnormally: ", child->name, error->message);
        g_error_free(error);
    }
    child->pid = 0;
    g_spawn_close_pid(pid);
}

static void
logind_manager_proxy_new_cb(GObject *source_object, GAsyncResult *res,
                            gpointer user_data)
{
    GError *error = NULL;

    logind_manager = logind_manager_proxy_new_for_bus_finish(res, &error);

    if (!logind_manager) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_printerr("GDBUS CALLBACK REACHED WITH CANCELLED STATE");
        g_printerr("Error connecting to systemd login manager: %s",
                   error->message);
        g_error_free(error);
        return;
    }
    logind_manager_call_get_session_by_pid(logind_manager, (guint)getpid(), NULL,
                                           logind_manager_call_get_session_cb, NULL);
    if (!opt_ignore_sleep) {
        take_sleep_delay_lock();
        g_signal_connect(logind_manager, "prepare-for-sleep",
                         G_CALLBACK(logind_manager_prepare_for_sleep_cb), NULL);
    }
}

static void
take_sleep_delay_lock(void)
{
    if (sleep_lock_fd >= 0)
        return;

    logind_manager_call_inhibit(logind_manager, "sleep", APP_NAME,
                                "Lock screen first", "delay", NULL,
                                logind_manager_call_inhibit_cb, NULL);
}

static void
logind_manager_call_inhibit_cb(GObject *source_object, GAsyncResult *res,
                               gpointer user_data)
{
    GVariant *lock_handle;
    GError *error = NULL;
    
    if (!logind_manager_call_inhibit_finish(logind_manager,
                                            &lock_handle, res, &error)) {
        g_printerr("Error taking sleep inhibitor lock: %s", error->message);
        g_error_free(error);
        return;
    }
    sleep_lock_fd = g_variant_get_handle(lock_handle);
    g_variant_unref(lock_handle);
}

static void
logind_manager_prepare_for_sleep_cb(gboolean active)
{
    if (active) {
        start_child(&locker);
        if (sleep_lock_fd >= 0) {
            close(sleep_lock_fd);
            sleep_lock_fd = -1;
        }
    } else
        take_sleep_delay_lock();
}

static void
logind_manager_call_get_session_cb(GObject *source_object, GAsyncResult *res,
                                   gpointer user_data)
{
    gchar *session_path;
    GError *error = NULL;

    if (!logind_manager_call_get_session_by_pid_finish(logind_manager,
                                                       &session_path,
                                                       res, &error)) {
        g_printerr("Error getting current session: %s", error->message);
        g_error_free(error);
        return;
    }
    logind_session_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
                                     G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                     LOGIND_SERVICE, session_path,
                                     NULL, logind_session_proxy_new_cb, NULL);
    g_free(session_path);
}

static void
logind_session_proxy_new_cb(GObject *source_object, GAsyncResult *res,
                            gpointer user_data)
{
    GError *error = NULL;

    logind_session = logind_session_proxy_new_for_bus_finish(res, &error);

    if (!logind_session) {
        g_printerr("Error connecting to session: %s", error->message);
        g_error_free(error);
        return;
    }
    g_signal_connect(logind_session, "lock",
                     G_CALLBACK(start_child), &locker);
    g_signal_connect(logind_session, "unlock",
                     G_CALLBACK(kill_child), &locker);
}

static gboolean
parse_options(int argc, char *argv[], GError **error)
{
    GOptionContext *opt_context;
    gboolean success;

    opt_context = g_option_context_new("- TODO");
    g_option_context_set_summary(opt_context,
                                 ""); // TODO
    g_option_context_add_main_entries(opt_context, opt_entries, NULL);
    success = g_option_context_parse(opt_context, &argc, &argv, error);
    g_option_context_free(opt_context);

    if (success && !locker.cmd) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                    "No %s specified", locker.name);
        success = FALSE;
    }
    return success;

}

static gboolean
parse_locker_cmd(const gchar *option_name, const gchar *value, gpointer data, GError **error)
{
    return parse_child_cmd(option_name, value, &locker, error);
}

static gboolean
parse_notifier_cmd(const gchar *option_name, const gchar *value, gpointer data, GError **error)
{
    return parse_child_cmd(option_name, value, &notifier, error);
}

static gboolean
parse_child_cmd(const gchar *option_name, const gchar *value, Child *child, GError **error)
{
    GError *parse_error = NULL;

    if (!g_shell_parse_argv(value, NULL, &child->cmd, &parse_error)) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                    "Error parsing argument for %s: %s",
                    option_name, parse_error->message);
        g_error_free(parse_error);
        return FALSE;
    }
    return TRUE;
}

static gboolean
reset_screensaver(xcb_connection_t *connection)
{
    if (!locker.pid)
        xcb_force_screen_saver(connection, XCB_SCREEN_SAVER_RESET);

    return TRUE;
}

static gboolean
exit_service(GMainLoop *loop)
{
    kill_child(&notifier);
    kill_child(&locker);
    g_main_loop_quit(loop);
    return TRUE;
}

int
main(int argc, char *argv[])
{
    GMainLoop *loop;
    GError *error = NULL;
    GCancellable *cancellable;
    xcb_connection_t *connection;
    int default_screen_number;
    const xcb_screen_t *default_screen;
    const xcb_query_extension_reply_t *extension_reply;

    setlocale(LC_ALL, "");
    
    if (!parse_options(argc, argv, &error))
        goto init_error;

    connection = xcb_connect(NULL, &default_screen_number);
    if (xcb_connection_has_error(connection)) {
        g_set_error(&error, XCB_ERROR, 0, "Connecting to X server failed");
        goto init_error;
    }

    xcb_prefetch_extension_data(connection, &xcb_screensaver_id);
    // TODO: create pixmap and atom

#if !GLIB_CHECK_VERSION(2, 36, 0)
    g_type_init();
#endif

    logind_manager_proxy_new_for_bus(G_BUS_TYPE_SYSTEM,
                                     G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                     LOGIND_SERVICE, LOGIND_PATH,
                                     cancellable, logind_manager_proxy_new_cb,
                                     NULL);

    g_unix_signal_add(SIGTERM, (GSourceFunc)exit_service, loop);
    g_unix_signal_add(SIGINT, (GSourceFunc)exit_service, loop);
    g_unix_signal_add(SIGHUP, (GSourceFunc)reset_screensaver, connection);

    extension_reply = xcb_get_extension_data(connection, &xcb_screensaver_id);
    if (!extension_reply || !extension_reply->present) {
        g_cancellable_cancel(cancellable);
        g_set_error(&error, XCB_ERROR, 0, "Screensaver extension unavailable");
        goto init_error;
    }

    default_screen = get_screen(connection, default_screen_number);
    if (!register_screensaver(connection, default_screen, &error))
        goto init_error;

    x_event_add(connection, (XEventFunc)screensaver_event_cb,
                (void *)&extension_reply->first_event);

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    xcb_screensaver_unset_attributes(connection, *default_screen);

    g_main_loop_unref(loop);
    if (sleep_lock_fd >= 0)
        close(sleep_lock_fd);

init_error:
    xcb_disconnect(connection);
    g_strfreev(notifier.cmd);
    g_strfreev(locker.cmd);

    if (error) {
        g_printerr("%s", error->message);
        g_error_free(error);
        exit(EXIT_FAILURE);
    }
    exit(EXIT_SUCCESS);
}