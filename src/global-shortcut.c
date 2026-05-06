#include "global-shortcut.h"

#include <gio/gio.h>
#include <stdio.h>

#define GLOBAL_SHORTCUT_ID "diction-scan-shortcut"
#define GLOBAL_SHORTCUT_TRIGGER "Super+Alt+L"

typedef enum {
    PORTAL_REQUEST_CREATE_SHORTCUT_SESSION = 1,
    PORTAL_REQUEST_BIND_SHORTCUTS = 2
} PortalRequestKind;

static GDBusConnection *global_shortcut_conn = NULL;
static char *global_shortcut_session_handle = NULL;
static guint global_shortcut_signal_sub_id = 0;
static guint global_shortcut_create_response_sub_id = 0;
static guint global_shortcut_bind_response_sub_id = 0;
static GlobalShortcutActivatedCallback activation_callback = NULL;
static gpointer activation_user_data = NULL;

static void dbus_global_shortcut_activated(GDBusConnection *connection,
                                           const gchar *sender_name,
                                           const gchar *object_path,
                                           const gchar *interface_name,
                                           const gchar *signal_name,
                                           GVariant *parameters,
                                           gpointer user_data) {
    (void)connection; (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name; (void)user_data;

    const char *session_handle = NULL;
    const char *shortcut_id = NULL;
    guint64 timestamp = 0;
    GVariant *options = NULL;
    g_variant_get(parameters, "(&o&st@a{sv})", &session_handle, &shortcut_id, &timestamp, &options);
    (void)timestamp;

    if (g_strcmp0(session_handle, global_shortcut_session_handle) == 0 &&
        g_strcmp0(shortcut_id, GLOBAL_SHORTCUT_ID) == 0) {
        
        const char *token = NULL;
        if (options && g_variant_lookup(options, "activation_token", "&s", &token) && token) {
            fprintf(stderr, "[Shortcut] Got token: %s\n", token);
        }

        fprintf(stderr, "[Shortcut] Triggering scan popup manual fetch!\n");
        if (activation_callback) {
            activation_callback(token, activation_user_data);
        }
    } else {
        fprintf(stderr, "[Shortcut] Ignoring unmatched ID: %s\n", shortcut_id ? shortcut_id : "null");
    }

    if (options) {
        g_variant_unref(options);
    }
}

static char *portal_sender_path_component(GDBusConnection *conn) {
    const char *unique_name = g_dbus_connection_get_unique_name(conn);
    if (!unique_name) {
        return NULL;
    }

    char *component = g_strdup(unique_name[0] == ':' ? unique_name + 1 : unique_name);
    g_strdelimit(component, ".", '_');
    return component;
}

static char *portal_token(const char *prefix) {
    return g_strdup_printf("diction_%s_%u", prefix, g_random_int());
}

static char *portal_request_path_for_token(GDBusConnection *conn, const char *token) {
    char *sender = portal_sender_path_component(conn);
    if (!sender) {
        return NULL;
    }

    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
    g_free(sender);
    return path;
}

static char *portal_session_path_for_token(GDBusConnection *conn, const char *token) {
    char *sender = portal_sender_path_component(conn);
    if (!sender) {
        return NULL;
    }

    char *path = g_strdup_printf("/org/freedesktop/portal/desktop/session/%s/%s", sender, token);
    g_free(sender);
    return path;
}

static void global_shortcut_request_response(GDBusConnection *connection,
                                             const gchar *sender_name,
                                             const gchar *object_path,
                                             const gchar *interface_name,
                                             const gchar *signal_name,
                                             GVariant *parameters,
                                             gpointer user_data);

static void global_shortcut_call_done(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    const char *method = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), res, &err);
    if (err) {
        g_warning("Global shortcut portal %s call failed: %s", method ? method : "unknown", err->message);
        g_error_free(err);
        return;
    }

    if (reply) {
        g_variant_unref(reply);
    }
}

static void bind_global_shortcut(void) {
    if (!global_shortcut_conn || !global_shortcut_session_handle) {
        return;
    }

    char *request_token = portal_token("bind");
    char *request_path = portal_request_path_for_token(global_shortcut_conn, request_token);
    if (!request_path) {
        g_free(request_token);
        return;
    }

    if (global_shortcut_bind_response_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(global_shortcut_conn, global_shortcut_bind_response_sub_id);
        global_shortcut_bind_response_sub_id = 0;
    }
    global_shortcut_bind_response_sub_id =
        g_dbus_connection_signal_subscribe(global_shortcut_conn,
                                           "org.freedesktop.portal.Desktop",
                                           "org.freedesktop.portal.Request",
                                           "Response",
                                           request_path,
                                           NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE,
                                           global_shortcut_request_response,
                                           GINT_TO_POINTER(PORTAL_REQUEST_BIND_SHORTCUTS),
                                           NULL);

    GVariantBuilder shortcut_props;
    g_variant_builder_init(&shortcut_props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&shortcut_props, "{sv}", "description",
                          g_variant_new_string("Scan selected text with Diction"));
    g_variant_builder_add(&shortcut_props, "{sv}", "preferred_trigger",
                          g_variant_new_string(GLOBAL_SHORTCUT_TRIGGER));

    GVariantBuilder shortcuts;
    g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
    g_variant_builder_add(&shortcuts, "(sa{sv})", GLOBAL_SHORTCUT_ID, &shortcut_props);

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(request_token));

    g_dbus_connection_call(global_shortcut_conn,
                           "org.freedesktop.portal.Desktop",
                           "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.GlobalShortcuts",
                           "BindShortcuts",
                           g_variant_new("(oa(sa{sv})sa{sv})",
                                         global_shortcut_session_handle,
                                         &shortcuts,
                                         "",
                                         &options),
                           G_VARIANT_TYPE("(o)"),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           global_shortcut_call_done,
                           "BindShortcuts");

    g_free(request_path);
    g_free(request_token);
}

static void global_shortcut_request_response(GDBusConnection *connection,
                                             const gchar *sender_name,
                                             const gchar *object_path,
                                             const gchar *interface_name,
                                             const gchar *signal_name,
                                             GVariant *parameters,
                                             gpointer user_data) {
    (void)sender_name; (void)object_path; (void)interface_name; (void)signal_name;

    PortalRequestKind kind = GPOINTER_TO_INT(user_data);
    guint response = 2;
    GVariant *results = NULL;
    g_variant_get(parameters, "(u@a{sv})", &response, &results);

    if (kind == PORTAL_REQUEST_CREATE_SHORTCUT_SESSION &&
        global_shortcut_create_response_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(connection, global_shortcut_create_response_sub_id);
        global_shortcut_create_response_sub_id = 0;
    } else if (kind == PORTAL_REQUEST_BIND_SHORTCUTS &&
               global_shortcut_bind_response_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(connection, global_shortcut_bind_response_sub_id);
        global_shortcut_bind_response_sub_id = 0;
    }

    if (response != 0) {
        g_warning("Global shortcut portal request failed or was cancelled (response %u)", response);
        if (results) {
            g_variant_unref(results);
        }
        return;
    }

    if (kind == PORTAL_REQUEST_CREATE_SHORTCUT_SESSION) {
        const char *session_handle = NULL;
        if (!results || !g_variant_lookup(results, "session_handle", "&s", &session_handle)) {
            g_warning("Global shortcut portal did not return a session_handle");
        } else {
            g_free(global_shortcut_session_handle);
            global_shortcut_session_handle = g_strdup(session_handle);

            if (global_shortcut_signal_sub_id == 0) {
                global_shortcut_signal_sub_id =
                    g_dbus_connection_signal_subscribe(global_shortcut_conn,
                                                       "org.freedesktop.portal.Desktop",
                                                       "org.freedesktop.portal.GlobalShortcuts",
                                                       "Activated",
                                                       "/org/freedesktop/portal/desktop",
                                                       global_shortcut_session_handle,
                                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                                       dbus_global_shortcut_activated,
                                                       NULL,
                                                       NULL);
            }

            bind_global_shortcut();
        }
    }

    if (results) {
        g_variant_unref(results);
    }
}

void global_shortcut_setup(GlobalShortcutActivatedCallback callback, gpointer user_data) {
    activation_callback = callback;
    activation_user_data = user_data;

    if (global_shortcut_conn) {
        return;
    }

    global_shortcut_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!global_shortcut_conn) {
        return;
    }

    char *request_token = portal_token("create");
    char *session_token = portal_token("session");
    char *request_path = portal_request_path_for_token(global_shortcut_conn, request_token);
    char *session_path = portal_session_path_for_token(global_shortcut_conn, session_token);
    if (!request_path || !session_path) {
        g_free(request_path);
        g_free(session_path);
        g_free(request_token);
        g_free(session_token);
        return;
    }

    global_shortcut_create_response_sub_id =
        g_dbus_connection_signal_subscribe(global_shortcut_conn,
                                           "org.freedesktop.portal.Desktop",
                                           "org.freedesktop.portal.Request",
                                           "Response",
                                           request_path,
                                           NULL,
                                           G_DBUS_SIGNAL_FLAGS_NONE,
                                           global_shortcut_request_response,
                                           GINT_TO_POINTER(PORTAL_REQUEST_CREATE_SHORTCUT_SESSION),
                                           NULL);

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(request_token));
    g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(session_token));

    g_dbus_connection_call(global_shortcut_conn,
                           "org.freedesktop.portal.Desktop",
                           "/org/freedesktop/portal/desktop",
                           "org.freedesktop.portal.GlobalShortcuts",
                           "CreateSession",
                           g_variant_new("(a{sv})", &options),
                           G_VARIANT_TYPE("(o)"),
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           NULL,
                           global_shortcut_call_done,
                           "CreateSession");

    g_free(request_path);
    g_free(session_path);
    g_free(request_token);
    g_free(session_token);
}

void global_shortcut_destroy(void) {
    if (!global_shortcut_conn) {
        g_clear_pointer(&global_shortcut_session_handle, g_free);
        return;
    }

    if (global_shortcut_create_response_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(global_shortcut_conn, global_shortcut_create_response_sub_id);
        global_shortcut_create_response_sub_id = 0;
    }
    if (global_shortcut_bind_response_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(global_shortcut_conn, global_shortcut_bind_response_sub_id);
        global_shortcut_bind_response_sub_id = 0;
    }
    if (global_shortcut_signal_sub_id != 0) {
        g_dbus_connection_signal_unsubscribe(global_shortcut_conn, global_shortcut_signal_sub_id);
        global_shortcut_signal_sub_id = 0;
    }

    if (global_shortcut_session_handle) {
        g_dbus_connection_call(global_shortcut_conn,
                               "org.freedesktop.portal.Desktop",
                               global_shortcut_session_handle,
                               "org.freedesktop.portal.Session",
                               "Close",
                               NULL,
                               NULL,
                               G_DBUS_CALL_FLAGS_NONE,
                               -1,
                               NULL,
                               NULL,
                               NULL);
    }

    g_clear_pointer(&global_shortcut_session_handle, g_free);
    g_clear_object(&global_shortcut_conn);
}
