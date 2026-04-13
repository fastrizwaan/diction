#include "tray-icon.h"
#include <gio/gio.h>
#include <string.h>

static GDBusConnection *dbus_conn = NULL;
static guint sni_id = 0;
static guint dbusmenu_id = 0;
static GtkApplication *app_ref = NULL;
static GtkWindow *main_win_ref = NULL;
static void (*scan_toggle_cb)(void) = NULL;
static void (*quit_app_cb)(void) = NULL;
static gboolean current_scan_active = FALSE;

/* SNI XML interface */
static const gchar sni_xml[] =
"<node>"
"  <interface name='org.kde.StatusNotifierItem'>"
"    <property name='Category' type='s' access='read'/>"
"    <property name='Id' type='s' access='read'/>"
"    <property name='Title' type='s' access='read'/>"
"    <property name='Status' type='s' access='read'/>"
"    <property name='WindowId' type='i' access='read'/>"
"    <property name='IconName' type='s' access='read'/>"
"    <property name='AttentionIconName' type='s' access='read'/>"
"    <property name='ItemIsMenu' type='b' access='read'/>"
"    <property name='Menu' type='o' access='read'/>"
"    <method name='ContextMenu'>"
"      <arg name='x' type='i' direction='in'/>"
"      <arg name='y' type='i' direction='in'/>"
"    </method>"
"    <method name='Activate'>"
"      <arg name='x' type='i' direction='in'/>"
"      <arg name='y' type='i' direction='in'/>"
"    </method>"
"    <method name='SecondaryActivate'>"
"      <arg name='x' type='i' direction='in'/>"
"      <arg name='y' type='i' direction='in'/>"
"    </method>"
"    <method name='Scroll'>"
"      <arg name='delta' type='i' direction='in'/>"
"      <arg name='orientation' type='s' direction='in'/>"
"    </method>"
"  </interface>"
"</node>";

/* DBusMenu XML interface */
static const gchar dbusmenu_xml[] =
"<node>"
"  <interface name='com.canonical.dbusmenu'>"
"    <property name='Version' type='u' access='read'/>"
"    <method name='GetLayout'>"
"      <arg name='parentId' type='i' direction='in'/>"
"      <arg name='recursionDepth' type='i' direction='in'/>"
"      <arg name='propertyNames' type='as' direction='in'/>"
"      <arg name='revision' type='u' direction='out'/>"
"      <arg name='layout' type='(ia{sv}av)' direction='out'/>"
"    </method>"
"    <method name='GetGroupProperties'>"
"      <arg name='ids' type='ai' direction='in'/>"
"      <arg name='propertyNames' type='as' direction='in'/>"
"      <arg name='properties' type='a(ia{sv})' direction='out'/>"
"    </method>"
"    <method name='GetProperty'>"
"      <arg name='id' type='i' direction='in'/>"
"      <arg name='name' type='s' direction='in'/>"
"      <arg name='value' type='v' direction='out'/>"
"    </method>"
"    <method name='Event'>"
"      <arg name='id' type='i' direction='in'/>"
"      <arg name='eventId' type='s' direction='in'/>"
"      <arg name='data' type='v' direction='in'/>"
"      <arg name='timestamp' type='u' direction='in'/>"
"    </method>"
"    <signal name='LayoutUpdated'>"
"      <arg name='revision' type='u'/>"
"      <arg name='parent' type='i'/>"
"    </signal>"
"  </interface>"
"</node>";

/* Build the DBusMenu layout */
static GVariant* build_dbusmenu_layout(void) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("(ia{sv}av)"));
    
    // Root item (id 0)
    g_variant_builder_add(&b, "i", 0);
    
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "a{sv}", &props);
    
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    
    // Item 1: Show/Hide
    GVariantBuilder i1;
    g_variant_builder_init(&i1, G_VARIANT_TYPE("(ia{sv}av)"));
    g_variant_builder_add(&i1, "i", 1);
    GVariantBuilder p1;
    g_variant_builder_init(&p1, G_VARIANT_TYPE("a{sv}"));
    gboolean is_visible = main_win_ref && gtk_widget_get_visible(GTK_WIDGET(main_win_ref));
    g_variant_builder_add(&p1, "{sv}", "label", g_variant_new_string(is_visible ? "Hide Diction" : "Show Diction"));
    g_variant_builder_add(&p1, "{sv}", "visible", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&p1, "{sv}", "enabled", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&i1, "a{sv}", &p1);
    GVariantBuilder c1;
    g_variant_builder_init(&c1, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&i1, "av", &c1);
    g_variant_builder_add(&children, "v", g_variant_builder_end(&i1));

    // Item 2: Enable Scan Popup
    GVariantBuilder i2;
    g_variant_builder_init(&i2, G_VARIANT_TYPE("(ia{sv}av)"));
    g_variant_builder_add(&i2, "i", 2);
    GVariantBuilder p2;
    g_variant_builder_init(&p2, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&p2, "{sv}", "label", g_variant_new_string("Scan Popup Enabled"));
    g_variant_builder_add(&p2, "{sv}", "toggle-type", g_variant_new_string("checkmark"));
    g_variant_builder_add(&p2, "{sv}", "toggle-state", g_variant_new_int32(current_scan_active ? 1 : 0));
    g_variant_builder_add(&p2, "{sv}", "visible", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&p2, "{sv}", "enabled", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&i2, "a{sv}", &p2);
    GVariantBuilder c2;
    g_variant_builder_init(&c2, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&i2, "av", &c2);
    g_variant_builder_add(&children, "v", g_variant_builder_end(&i2));

    // Item 3: Separator
    GVariantBuilder i3;
    g_variant_builder_init(&i3, G_VARIANT_TYPE("(ia{sv}av)"));
    g_variant_builder_add(&i3, "i", 3);
    GVariantBuilder p3;
    g_variant_builder_init(&p3, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&p3, "{sv}", "type", g_variant_new_string("separator"));
    g_variant_builder_add(&i3, "a{sv}", &p3);
    GVariantBuilder c3;
    g_variant_builder_init(&c3, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&i3, "av", &c3);
    g_variant_builder_add(&children, "v", g_variant_builder_end(&i3));

    // Item 4: Quit
    GVariantBuilder i4;
    g_variant_builder_init(&i4, G_VARIANT_TYPE("(ia{sv}av)"));
    g_variant_builder_add(&i4, "i", 4);
    GVariantBuilder p4;
    g_variant_builder_init(&p4, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&p4, "{sv}", "label", g_variant_new_string("Quit"));
    g_variant_builder_add(&p4, "{sv}", "visible", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&p4, "{sv}", "enabled", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&i4, "a{sv}", &p4);
    GVariantBuilder c4;
    g_variant_builder_init(&c4, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&i4, "av", &c4);
    g_variant_builder_add(&children, "v", g_variant_builder_end(&i4));
    
    g_variant_builder_add(&b, "av", &children);
    return g_variant_builder_end(&b);
}

static void dbusmenu_handle_method(GDBusConnection *connection,
                                   const gchar *sender,
                                   const gchar *object_path,
                                   const gchar *interface_name,
                                   const gchar *method_name,
                                   GVariant *parameters,
                                   GDBusMethodInvocation *invocation,
                                   gpointer user_data) {
    (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)user_data;
    
    if (g_strcmp0(method_name, "GetLayout") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@*)", 1, build_dbusmenu_layout()));
    } else if (g_strcmp0(method_name, "GetGroupProperties") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &b));
    } else if (g_strcmp0(method_name, "GetProperty") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", g_variant_new_string("")));
    } else if (g_strcmp0(method_name, "Event") == 0) {
        gint32 id;
        const gchar *event_id;
        g_variant_get(parameters, "(isvu)", &id, &event_id, NULL, NULL);
        if (g_strcmp0(event_id, "clicked") == 0) {
            if (id == 1) {
                // Show/Hide
                if (main_win_ref) {
                    if (gtk_widget_get_visible(GTK_WIDGET(main_win_ref))) {
                        gtk_widget_hide(GTK_WIDGET(main_win_ref));
                    } else {
                        gtk_window_present(main_win_ref);
                    }
                }
            } else if (id == 2) {
                // Toggle scan
                if (scan_toggle_cb) {
                    scan_toggle_cb();
                }
            } else if (id == 4) {
                // Quit
                if (quit_app_cb) {
                    quit_app_cb();
                }
            }
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
    }
}

static GVariant* dbusmenu_get_property(GDBusConnection *connection,
                                       const gchar *sender,
                                       const gchar *object_path,
                                       const gchar *interface_name,
                                       const gchar *property_name,
                                       GError **error,
                                       gpointer user_data) {
    (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)user_data; (void)error;
    if (g_strcmp0(property_name, "Version") == 0) {
        return g_variant_new_uint32(3);
    }
    return NULL;
}

static const GDBusInterfaceVTable dbusmenu_vtable = {
    dbusmenu_handle_method,
    dbusmenu_get_property,
    NULL,
    {0}
};

/* SNI interface methods */
static void sni_handle_method(GDBusConnection *connection,
                              const gchar *sender,
                              const gchar *object_path,
                              const gchar *interface_name,
                              const gchar *method_name,
                              GVariant *parameters,
                              GDBusMethodInvocation *invocation,
                              gpointer user_data) {
    (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)user_data; (void)parameters;

    if (g_strcmp0(method_name, "Activate") == 0) {
        if (main_win_ref) {
            if (gtk_widget_get_visible(GTK_WIDGET(main_win_ref))) {
                gtk_widget_hide(GTK_WIDGET(main_win_ref));
            } else {
                gtk_window_present(main_win_ref);
            }
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (g_strcmp0(method_name, "ContextMenu") == 0 ||
               g_strcmp0(method_name, "SecondaryActivate") == 0 ||
               g_strcmp0(method_name, "Scroll") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown method");
    }
}

static GVariant* sni_get_property(GDBusConnection *connection,
                                  const gchar *sender,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *property_name,
                                  GError **error,
                                  gpointer user_data) {
    (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)user_data; (void)error;
    if (g_strcmp0(property_name, "Category") == 0) return g_variant_new_string("ApplicationStatus");
    if (g_strcmp0(property_name, "Id") == 0) return g_variant_new_string("diction");
    if (g_strcmp0(property_name, "Title") == 0) return g_variant_new_string("Diction");
    if (g_strcmp0(property_name, "Status") == 0) return g_variant_new_string("Active");
    if (g_strcmp0(property_name, "IconName") == 0) return g_variant_new_string("io.github.fastrizwaan.diction");
    if (g_strcmp0(property_name, "ItemIsMenu") == 0) return g_variant_new_boolean(FALSE);
    if (g_strcmp0(property_name, "Menu") == 0) {
        char path[128];
        snprintf(path, sizeof(path), "/org/ayatana/NotificationItem/diction/Menu");
        return g_variant_new_object_path(path);
    }
    return NULL;
}

static const GDBusInterfaceVTable sni_vtable = {
    sni_handle_method,
    sni_get_property,
    NULL,
    {0}
};

static void register_sni_with_watcher(void) {
    if (!dbus_conn) return;

    /* KDE / Freedesktop StatusNotifierWatcher */
    char obj_path[128];
    snprintf(obj_path, sizeof(obj_path), "/org/ayatana/NotificationItem/diction");

    GVariant *args = g_variant_new("(s)", obj_path);
    
    // Attempt KDE watcher first
    g_dbus_connection_call(dbus_conn,
        "org.kde.StatusNotifierWatcher",
        "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher",
        "RegisterStatusNotifierItem",
        args,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        NULL, NULL);
}

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)name; (void)user_data;
    dbus_conn = g_object_ref(connection);

    GError *err = NULL;
    GDBusNodeInfo *sni_info = g_dbus_node_info_new_for_xml(sni_xml, &err);
    if (!sni_info) {
        g_warning("Failed to parse SNI XML: %s", err->message);
        g_clear_error(&err);
        return;
    }
    
    GDBusNodeInfo *dbusmenu_info = g_dbus_node_info_new_for_xml(dbusmenu_xml, &err);
    if (!dbusmenu_info) {
        g_warning("Failed to parse DBusMenu XML: %s", err->message);
        g_clear_error(&err);
        g_dbus_node_info_unref(sni_info);
        return;
    }

    char base_path[128];
    snprintf(base_path, sizeof(base_path), "/org/ayatana/NotificationItem/diction");

    sni_id = g_dbus_connection_register_object(connection, base_path, sni_info->interfaces[0], &sni_vtable, NULL, NULL, &err);
    if (!sni_id) {
        g_warning("Failed to register SNI object: %s", err->message);
        g_clear_error(&err);
    }

    char menu_path[128];
    snprintf(menu_path, sizeof(menu_path), "%s/Menu", base_path);
    dbusmenu_id = g_dbus_connection_register_object(connection, menu_path, dbusmenu_info->interfaces[0], &dbusmenu_vtable, NULL, NULL, &err);
    if (!dbusmenu_id) {
        g_warning("Failed to register DBusMenu object: %s", err->message);
        g_clear_error(&err);
    }

    g_dbus_node_info_unref(sni_info);
    g_dbus_node_info_unref(dbusmenu_info);
    
    register_sni_with_watcher();
}

static guint bus_owner_id = 0;

void tray_icon_init(GtkApplication *app, GtkWindow *main_window,
                    void (*toggle_scan_cb)(void),
                    void (*quit_cb)(void)) {
    if (bus_owner_id != 0) return; // already init

    app_ref = app;
    main_win_ref = main_window;
    scan_toggle_cb = toggle_scan_cb;
    quit_app_cb = quit_cb;

    char bus_name[128];
    snprintf(bus_name, sizeof(bus_name), "org.kde.StatusNotifierItem-%d-1", getpid());

    bus_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                                  bus_name,
                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                  on_bus_acquired,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL);
}

void tray_icon_destroy(void) {
    if (bus_owner_id != 0) {
        g_bus_unown_name(bus_owner_id);
        bus_owner_id = 0;
    }
    if (dbus_conn) {
        g_object_unref(dbus_conn);
        dbus_conn = NULL;
    }
    sni_id = 0;
    dbusmenu_id = 0;
    app_ref = NULL;
    main_win_ref = NULL;
}

void tray_icon_set_scan_active(gboolean active) {
    current_scan_active = active;
    if (dbus_conn && dbusmenu_id) {
        char menu_path[128];
        snprintf(menu_path, sizeof(menu_path), "/org/ayatana/NotificationItem/diction/Menu");
        g_dbus_connection_emit_signal(dbus_conn,
                                      NULL,
                                      menu_path,
                                      "com.canonical.dbusmenu",
                                      "LayoutUpdated",
                                      g_variant_new("(ui)", 1, 0),
                                      NULL);
    }
}
