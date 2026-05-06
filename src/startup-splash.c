#include "startup-splash.h"

#include <adwaita.h>

static GtkWindow *startup_splash_window = NULL;
static GtkLabel *startup_splash_status_label = NULL;
static GtkLabel *startup_splash_count_label = NULL;
static GtkProgressBar *startup_splash_progress = NULL;
static guint startup_splash_pulse_id = 0;
static gboolean startup_loading_active = FALSE;

static gboolean pulse_startup_splash(gpointer user_data) {
    (void)user_data;
    if (!startup_loading_active || !startup_splash_progress) {
        startup_splash_pulse_id = 0;
        return G_SOURCE_REMOVE;
    }

    gtk_progress_bar_pulse(startup_splash_progress);
    return G_SOURCE_CONTINUE;
}

static void ensure_startup_splash_pulsing(void) {
    if (startup_splash_pulse_id != 0) {
        return;
    }

    if (!startup_splash_progress) {
        return;
    }

    gtk_progress_bar_set_pulse_step(startup_splash_progress, 0.08);
    startup_splash_pulse_id = g_timeout_add(80, pulse_startup_splash, NULL);
}

static void stop_startup_splash_pulsing(void) {
    if (startup_splash_pulse_id != 0) {
        g_source_remove(startup_splash_pulse_id);
        startup_splash_pulse_id = 0;
    }
}

void startup_splash_update_progress(guint completed, guint total, const char *status_text) {
    if (!startup_splash_window) {
        return;
    }

    if (startup_splash_status_label) {
        if (status_text && *status_text) {
            gtk_label_set_text(startup_splash_status_label, status_text);
        } else if (total > 0) {
            char *fallback = g_strdup_printf("Loading dictionaries... %u/%u", completed, total);
            gtk_label_set_text(startup_splash_status_label, fallback);
            g_free(fallback);
        } else {
            gtk_label_set_text(startup_splash_status_label, "Preparing dictionary library...");
        }
    }

    if (!startup_splash_progress) {
        return;
    }

    if (total > 0) {
        if (startup_splash_count_label) {
            char *count = g_strdup_printf("%u of %u", completed, total);
            gtk_label_set_text(startup_splash_count_label, count);
            g_free(count);
        }
        stop_startup_splash_pulsing();
        gtk_progress_bar_set_fraction(startup_splash_progress,
            CLAMP((gdouble)completed / (gdouble)MAX(total, 1), 0.0, 1.0));
    } else {
        if (startup_splash_count_label) {
            gtk_label_set_text(startup_splash_count_label, "Scanning...");
        }
        gtk_progress_bar_set_fraction(startup_splash_progress, 0.0);
        ensure_startup_splash_pulsing();
    }
}

static GtkWidget *create_startup_splash_logo(void) {
    char *cwd = g_get_current_dir();
    char *icon_path = g_build_filename(cwd,
                                       "data", "icons",
                                       "io.github.fastrizwaan.diction.svg",
                                       NULL);
    GtkWidget *image = NULL;

    if (g_file_test(icon_path, G_FILE_TEST_EXISTS)) {
        image = gtk_image_new_from_file(icon_path);
    } else {
        image = gtk_image_new_from_icon_name("io.github.fastrizwaan.diction");
    }

    gtk_image_set_pixel_size(GTK_IMAGE(image), 54);
    gtk_widget_add_css_class(image, "startup-logo");
    gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(image, GTK_ALIGN_CENTER);
    g_free(cwd);
    g_free(icon_path);
    return image;
}

static void ensure_startup_splash_css(void) {
    static GtkCssProvider *provider = NULL;
    if (provider) {
        return;
    }

    provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        "window.startup-splash,"
        "window.startup-splash.background,"
        "window.startup-splash > contents {"
        "  background: transparent;"
        "  background-color: transparent;"
        "}"
        ".startup-shell {"
        "  margin: 0;"
        "  border-radius: 30px;"
        "  padding: 30px 34px 28px 34px;"
        "  border: 1px solid alpha(@accent_bg_color, 0.18);"
        "  background: @window_bg_color;"
        "  box-shadow: none;"
        "  overflow: hidden;"
        "}"
        ".startup-logo-wrap {"
        "  min-width: 78px;"
        "  min-height: 78px;"
        "  border-radius: 20px;"
        "  background: alpha(@accent_bg_color, 0.10);"
        "  border: 1px solid alpha(@accent_bg_color, 0.10);"
        "  box-shadow: inset 0 1px 0 rgba(255,255,255,0.18);"
        "}"
        ".startup-logo {"
        "  -gtk-icon-shadow: 0 1px 2px rgba(0,0,0,0.12);"
        "}"
        ".startup-title {"
        "  font-size: 1.65rem;"
        "  font-weight: 800;"
        "  letter-spacing: 0;"
        "  color: @accent_color;"
        "}"
        ".startup-subtitle {"
        "  opacity: 0.72;"
        "  font-size: 0.96rem;"
        "  line-height: 1.4;"
        "}"
        ".startup-meta {"
        "  min-height: 24px;"
        "  margin-top: 24px;"
        "}"
        ".startup-status {"
        "  opacity: 0.86;"
        "  font-size: 0.94rem;"
        "  font-weight: 500;"
        "}"
        ".startup-count {"
        "  opacity: 0.65;"
        "  font-size: 0.88rem;"
        "  font-variant-numeric: tabular-nums;"
        "}"
        ".startup-progress trough {"
        "  background: alpha(@accent_bg_color, 0.10);"
        "  border: none;"
        "  border-radius: 999px;"
        "  min-height: 7px;"
        "}"
        ".startup-progress progress {"
        "  background: @accent_bg_color;"
        "  border: none;"
        "  min-height: 7px;"
        "  border-radius: 999px;"
        "}"
        ".startup-stop-btn {"
        "  min-width: 34px;"
        "  min-height: 34px;"
        "  padding: 0;"
        "  border-radius: 999px;"
        "  color: alpha(@window_fg_color, 0.62);"
        "  background: transparent;"
        "  transition: background 0.16s ease, color 0.16s ease;"
        "}"
        ".startup-stop-btn:hover {"
        "  background: alpha(@error_bg_color, 0.13);"
        "  color: @error_color;"
        "}");

    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

void startup_splash_show(GtkApplication *app, GtkWindow *parent, GCallback cancel_callback) {
    if (startup_splash_window) {
        return;
    }

    ensure_startup_splash_css();

    GtkWindow *window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_application(window, app);
    gtk_window_set_title(window, "Diction");
    gtk_window_set_default_size(window, 476, 180);
    gtk_window_set_resizable(window, FALSE);
    gtk_window_set_decorated(window, FALSE);
    gtk_window_set_modal(window, FALSE);
    gtk_window_set_hide_on_close(window, TRUE);
    gtk_window_set_icon_name(window, "io.github.fastrizwaan.diction");
    if (parent) {
        gtk_window_set_transient_for(window, parent);
    }
    gtk_widget_add_css_class(GTK_WIDGET(window), "startup-splash");

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(outer, "startup-shell");
    gtk_window_set_child(window, outer);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    gtk_box_append(GTK_BOX(outer), header);

    GtkWidget *logo_wrap = gtk_center_box_new();
    gtk_widget_add_css_class(logo_wrap, "startup-logo-wrap");
    gtk_widget_set_size_request(logo_wrap, 78, 78);
    gtk_widget_set_halign(logo_wrap, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(logo_wrap, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header), logo_wrap);
    gtk_center_box_set_center_widget(GTK_CENTER_BOX(logo_wrap), create_startup_splash_logo());

    GtkWidget *copy = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(copy, TRUE);
    gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header), copy);

    GtkWidget *title = gtk_label_new("Diction");
    gtk_widget_add_css_class(title, "startup-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_box_append(GTK_BOX(copy), title);

    GtkWidget *stop_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(stop_btn, "flat");
    gtk_widget_add_css_class(stop_btn, "circular");
    gtk_widget_add_css_class(stop_btn, "startup-stop-btn");
    gtk_widget_set_valign(stop_btn, GTK_ALIGN_START);
    gtk_widget_set_halign(stop_btn, GTK_ALIGN_END);
    gtk_widget_set_tooltip_text(stop_btn, "Stop and Load App");
    if (cancel_callback) {
        g_signal_connect(stop_btn, "clicked", cancel_callback, NULL);
    }
    gtk_box_append(GTK_BOX(header), stop_btn);

    GtkWidget *subtitle = gtk_label_new("A fast and lightweight dictionary application");
    gtk_widget_add_css_class(subtitle, "startup-subtitle");
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
    gtk_box_append(GTK_BOX(copy), subtitle);

    GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(meta, "startup-meta");
    gtk_widget_set_margin_top(meta, 14);
    gtk_box_append(GTK_BOX(outer), meta);

    startup_splash_status_label = GTK_LABEL(gtk_label_new("Preparing dictionary library..."));
    gtk_widget_add_css_class(GTK_WIDGET(startup_splash_status_label), "startup-status");
    gtk_label_set_xalign(startup_splash_status_label, 0.0f);
    gtk_label_set_ellipsize(startup_splash_status_label, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(startup_splash_status_label), TRUE);
    gtk_box_append(GTK_BOX(meta), GTK_WIDGET(startup_splash_status_label));

    startup_splash_count_label = GTK_LABEL(gtk_label_new("Scanning..."));
    gtk_widget_add_css_class(GTK_WIDGET(startup_splash_count_label), "startup-count");
    gtk_label_set_xalign(startup_splash_count_label, 1.0f);
    gtk_box_append(GTK_BOX(meta), GTK_WIDGET(startup_splash_count_label));

    startup_splash_progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_add_css_class(GTK_WIDGET(startup_splash_progress), "startup-progress");
    gtk_widget_set_margin_top(GTK_WIDGET(startup_splash_progress), 12);
    gtk_box_append(GTK_BOX(outer), GTK_WIDGET(startup_splash_progress));

    startup_splash_window = window;
    startup_loading_active = TRUE;
    startup_splash_update_progress(0, 0, "Preparing dictionary library...");
    gtk_window_present(window);
}

void startup_splash_close(void) {
    stop_startup_splash_pulsing();
    startup_loading_active = FALSE;

    if (startup_splash_window) {
        gtk_window_destroy(startup_splash_window);
    }

    startup_splash_window = NULL;
    startup_splash_status_label = NULL;
    startup_splash_count_label = NULL;
    startup_splash_progress = NULL;
}

gboolean startup_splash_is_active(void) {
    return startup_loading_active;
}
