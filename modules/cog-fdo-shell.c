/*
 * cog-fdo-shell.c
 *
 * Copyright (C) 2020 Igalia S.L.
 * Copyright (C) 2018-2019 Adrian Perez de Castro <aperez@igalia.com>
 * Copyright (C) 2018 Eduardo Lima <elima@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include <cog.h>
#include <wpe/wpe.h>
#include "../platform/pwl.h"

#include <errno.h>
#include <stdint.h>
#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include <string.h>


#define DEFAULT_ZOOM_STEP 0.1f

#if 1
# define TRACE(fmt, ...) g_debug ("%s: " fmt, G_STRFUNC, ##__VA_ARGS__)
#else
# define TRACE(fmt, ...) ((void) 0)
#endif

extern PwlData wl_data;
extern PwlXKBData xkb_data;

const static int wpe_view_activity_state_initiated = 1 << 4;

static PwlDisplay *s_pdisplay = NULL;
static PwlWinData *s_win_data = NULL;

static bool s_support_checked = false;
G_LOCK_DEFINE_STATIC (s_globals);

typedef struct {
    CogShellClass parent_class;
} CogFdoShellClass;

typedef struct {
    CogShell parent;
} CogFdoShell;

typedef struct {
    CogShell *shell;
    PwlData  *wl_data;
    PwlWinData  *win_data;
} CogFdoShellCallbackData;

static CogFdoShellCallbackData *s_callback_userdata = NULL;

static void cog_fdo_shell_initable_iface_init (GInitableIface *iface);

struct wpe_input_touch_event_raw touch_points[10];

GSList* wpe_host_data_exportable = NULL;

static struct {
    struct wpe_fdo_egl_exported_image *image;
} wpe_view_data = {NULL, };

typedef struct {
    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_view_backend *backend;
    struct wpe_fdo_egl_exported_image *image;
} WpeViewBackendData;

void
wpe_view_backend_data_free (WpeViewBackendData *data)
{
    wpe_view_backend_exportable_fdo_destroy (data->exportable);
    /* TODO: Either free data->backend or make sure it is freed elsewhere. */
    /* TODO: I think the data->image is actually freed already when it's not needed anymore. */
    g_free (data);
}


/* Output scale */
#if HAVE_DEVICE_SCALING
static void
noop()
{
}

static void
output_handle_scale (void *data,
                     struct wl_output *output,
                     int32_t factor)
{
    bool found = false;
    for (int i = 0; i < G_N_ELEMENTS (wl_data.metrics); i++)
    {
        if (wl_data.metrics[i].output == output) {
            found = true;
            wl_data.metrics[i].scale = factor;
            break;
        }
    }
    if (!found)
    {
        g_warning ("Unknown output %p\n", output);
        return;
    }
    g_info ("Got scale factor %i for output %p\n", factor, output);
}

struct wpe_view_backend*
cog_shell_get_active_wpe_backend (CogShell *shell)
{
    WebKitWebView *webview = WEBKIT_WEB_VIEW(cog_shell_get_active_view (shell));

    return webkit_web_view_backend_get_wpe_backend (webkit_web_view_get_backend (webview));
}

static void
on_surface_enter (PwlDisplay* display, void *userdata)
{
    CogFdoShellCallbackData *cb_userdata = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (cb_userdata->shell);
    wpe_view_backend_dispatch_set_device_scale_factor (backend, wl_data.current_output.scale);
}
#endif /* HAVE_DEVICE_SCALING */


/* Pointer */
static void
on_pointer_on_motion (PwlDisplay* display, void *userdata)
{
    CogFdoShellCallbackData *cb_userdata = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (cb_userdata->shell);
    struct wpe_input_pointer_event event = {
        wpe_input_pointer_event_type_motion,
        cb_userdata->wl_data->pointer.time,
        cb_userdata->wl_data->pointer.x * cb_userdata->wl_data->current_output.scale,
        cb_userdata->wl_data->pointer.y * cb_userdata->wl_data->current_output.scale,
        cb_userdata->wl_data->pointer.button,
        cb_userdata->wl_data->pointer.state
    };
    wpe_view_backend_dispatch_pointer_event (backend, &event);
}

static void
on_pointer_on_button (PwlDisplay* display, void *userdata)
{
    CogFdoShellCallbackData *cb_userdata = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (cb_userdata->shell);
    struct wpe_input_pointer_event event = {
        wpe_input_pointer_event_type_button,
        cb_userdata->wl_data->pointer.time,
        cb_userdata->wl_data->pointer.x * cb_userdata->wl_data->current_output.scale,
        cb_userdata->wl_data->pointer.y * cb_userdata->wl_data->current_output.scale,
        cb_userdata->wl_data->pointer.button,
        cb_userdata->wl_data->pointer.state,
    };
    wpe_view_backend_dispatch_pointer_event (backend, &event);
}

static void
on_pointer_on_axis (PwlDisplay* display, void *userdata)
{
    CogFdoShellCallbackData *cb_userdata = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (cb_userdata->shell);
    struct wpe_input_axis_event event = {
        wpe_input_axis_event_type_motion,
        cb_userdata->wl_data->pointer.time,
        cb_userdata->wl_data->pointer.x * cb_userdata->wl_data->current_output.scale,
        cb_userdata->wl_data->pointer.y * cb_userdata->wl_data->current_output.scale,
        cb_userdata->wl_data->pointer.axis,
        cb_userdata->wl_data->pointer.value,
    };
    wpe_view_backend_dispatch_axis_event (backend, &event);
}

/* Touch */
static void
on_touch_on_down (PwlDisplay* display, void *userdata)
{
    CogFdoShellCallbackData *cb_userdata = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (cb_userdata->shell);
    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_down,
        cb_userdata->wl_data->touch.time,
        cb_userdata->wl_data->touch.id,
        wl_fixed_to_int (cb_userdata->wl_data->touch.x) * cb_userdata->wl_data->current_output.scale,
        wl_fixed_to_int (cb_userdata->wl_data->touch.y) * cb_userdata->wl_data->current_output.scale,
    };

    memcpy (&touch_points[cb_userdata->wl_data->touch.id],
            &raw_event,
            sizeof (struct wpe_input_touch_event_raw));

    struct wpe_input_touch_event event = {
        touch_points,
        10,
        raw_event.type,
        raw_event.id,
        raw_event.time
    };

    wpe_view_backend_dispatch_touch_event (backend, &event);
}

static void
on_touch_on_up (PwlDisplay* display, void *userdata)
{
    CogFdoShellCallbackData *cb_userdata = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (cb_userdata->shell);
    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_up,
        cb_userdata->wl_data->touch.time,
        cb_userdata->wl_data->touch.id,
        touch_points[wl_data.touch.id].x,
        touch_points[wl_data.touch.id].y,
    };

    memcpy (&touch_points[cb_userdata->wl_data->touch.id],
            &raw_event,
            sizeof (struct wpe_input_touch_event_raw));

    struct wpe_input_touch_event event = {
        touch_points,
        10,
        raw_event.type,
        raw_event.id,
        raw_event.time
    };

    wpe_view_backend_dispatch_touch_event (backend, &event);

    memset (&touch_points[cb_userdata->wl_data->touch.id],
            0x00,
            sizeof (struct wpe_input_touch_event_raw));
}

static void
on_touch_on_motion (PwlDisplay* display, void *userdata)
{
    CogFdoShellCallbackData *cb_userdata = userdata;

    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (cb_userdata->shell);

    struct wpe_input_touch_event_raw raw_event = {
        wpe_input_touch_event_type_motion,
        cb_userdata->wl_data->touch.time,
        cb_userdata->wl_data->touch.id,
        wl_fixed_to_int (cb_userdata->wl_data->touch.x) * cb_userdata->wl_data->current_output.scale,
        wl_fixed_to_int (cb_userdata->wl_data->touch.y) * cb_userdata->wl_data->current_output.scale,
    };

    memcpy (&touch_points[cb_userdata->wl_data->touch.id],
            &raw_event,
            sizeof (struct wpe_input_touch_event_raw));

    struct wpe_input_touch_event event = {
        touch_points,
        10,
        raw_event.type,
        raw_event.id,
        raw_event.time
    };

    wpe_view_backend_dispatch_touch_event (backend, &event);
}

G_DEFINE_DYNAMIC_TYPE_EXTENDED (CogFdoShell, cog_fdo_shell, COG_TYPE_SHELL, 0,
    g_io_extension_point_implement (COG_MODULES_SHELL_EXTENSION_POINT,
                                    g_define_type_id, "fdo", 100);
    G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_INITABLE, cog_fdo_shell_initable_iface_init))

G_MODULE_EXPORT
void
g_io_fdo_shell_load (GIOModule *module)
{
    TRACE ("");
    cog_fdo_shell_register_type (G_TYPE_MODULE (module));
}

G_MODULE_EXPORT
void
g_io_fdo_shell_unload (GIOModule *module G_GNUC_UNUSED)
{
    TRACE ("");

    G_LOCK (s_globals);
    g_clear_pointer (&s_pdisplay, pwl_display_destroy);
    G_UNLOCK (s_globals);
}

static gboolean
cog_fdo_shell_is_supported (void)
{
    gboolean result;

    G_LOCK (s_globals);

    if (!s_support_checked) {
        g_autoptr(GError) error = NULL;
        s_pdisplay = pwl_display_connect (NULL, &error);
        if (!s_pdisplay)
            g_debug ("%s: %s", G_STRFUNC, error->message);
        s_support_checked = true;
    }
    TRACE ("PwlDisplay @ %p", s_pdisplay);
    result = s_pdisplay ? TRUE : FALSE;

    G_UNLOCK (s_globals);

    return result;
}

WebKitWebViewBackend* cog_shell_new_fdo_view_backend (CogShell *shell);
WebKitWebViewBackend* cog_shell_fdo_resume_active_views (CogShell *shell);

static void
cog_fdo_shell_class_init (CogFdoShellClass *klass)
{
    TRACE ("");
    CogShellClass *shell_class = COG_SHELL_CLASS (klass);
    shell_class->is_supported = cog_fdo_shell_is_supported;
    shell_class->cog_shell_new_view_backend = cog_shell_new_fdo_view_backend;
    shell_class->cog_shell_resume_active_views = cog_shell_fdo_resume_active_views;
}

static void
cog_fdo_shell_class_finalize (CogFdoShellClass *klass)
{
    TRACE ("");
    /* TODO
    g_assert (platform);

    // free WPE view data
    if (wpe_view_data.frame_callback != NULL)
        wl_callback_destroy (wpe_view_data.frame_callback);
    if (wpe_view_data.image != NULL) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (wpe_host_data.exportable,
                                                                             wpe_view_data.image);
    }
    g_clear_pointer (&wpe_view_data.buffer, wl_buffer_destroy);

    / * @FIXME: check why this segfaults
    wpe_view_backend_destroy (wpe_view_data.backend);
    * /

    / * free WPE host data * /
    / * @FIXME: check why this segfaults
    wpe_view_backend_exportable_fdo_destroy (wpe_host_data.exportable);
    * /
*/
    g_free(s_callback_userdata);

    clear_input (s_pdisplay);

    destroy_window (s_pdisplay, s_win_data);
    pwl_display_egl_deinit (s_pdisplay);
}


static void
on_window_resize (PwlWinData *win_data, void *userdata)
{
    CogFdoShellCallbackData *cb_userdata = userdata;
    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (cb_userdata->shell);
    wpe_view_backend_dispatch_set_size (backend,
                                        win_data->width,
                                        win_data->height);
}


static bool
on_capture_app_key (PwlDisplay *display, void *userdata)
{
    CogFdoShellCallbackData *cb_userdata = userdata;
    CogLauncher *launcher = cog_launcher_get_default ();
    WebKitWebView *web_view = WEBKIT_WEB_VIEW(cog_shell_get_active_view (cog_launcher_get_shell (launcher)));

    uint32_t keysym = cb_userdata->wl_data->keyboard.event.keysym;
    uint32_t unicode = cb_userdata->wl_data->keyboard.event.unicode;
    uint32_t state = cb_userdata->wl_data->keyboard.event.state;
    uint8_t modifiers =cb_userdata->wl_data->keyboard.event.modifiers;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        /* Ctrl+W, exit the application */
        if (modifiers == wpe_input_keyboard_modifier_control &&
                 unicode == 0x17 && keysym == 0x77) {
            // TODO: A loadable module in a library, we shouldn't be causing
            // the application to quit, this should be decided by the application.
            // Not even sure that it's a good idea to handle any key shortcuts.
            g_application_quit (G_APPLICATION (launcher));
            return true;
        }
        /* Ctrl+Plus, zoom in */
        else if (modifiers == wpe_input_keyboard_modifier_control &&
                 unicode == XKB_KEY_equal && keysym == XKB_KEY_equal) {
            const double level = webkit_web_view_get_zoom_level (web_view);
            webkit_web_view_set_zoom_level (web_view,
                                            level + DEFAULT_ZOOM_STEP);
            return true;
        }
        /* Ctrl+Minus, zoom out */
        else if (modifiers == wpe_input_keyboard_modifier_control &&
                 unicode == 0x2D && keysym == 0x2D) {
            const double level = webkit_web_view_get_zoom_level (web_view);
            webkit_web_view_set_zoom_level (web_view,
                                            level - DEFAULT_ZOOM_STEP);
            return true;
        }
        /* Ctrl+0, restore zoom level to 1.0 */
        else if (modifiers == wpe_input_keyboard_modifier_control &&
                 unicode == XKB_KEY_0 && keysym == XKB_KEY_0) {
            webkit_web_view_set_zoom_level (web_view, 1.0f);
            return true;
        }
        /* Alt+Left, navigate back */
        else if (modifiers == wpe_input_keyboard_modifier_alt &&
                 unicode == 0 && keysym == XKB_KEY_Left) {
            webkit_web_view_go_back (web_view);
            return true;
        }
        /* Alt+Right, navigate forward */
        else if (modifiers == wpe_input_keyboard_modifier_alt &&
                 unicode == 0 && keysym == XKB_KEY_Right) {
            webkit_web_view_go_forward (web_view);
            return true;
        }
    }

    return false;
}


static void
on_key_event (PwlDisplay* display, void *userdata)
{
    CogFdoShellCallbackData *cb_userdata = userdata;

    struct wpe_view_backend* backend = cog_shell_get_active_wpe_backend (cb_userdata->shell);

    struct wpe_input_keyboard_event event = {
        cb_userdata->wl_data->keyboard.event.timestamp,
        cb_userdata->wl_data->keyboard.event.keysym,
        cb_userdata->wl_data->keyboard.event.unicode,
        cb_userdata->wl_data->keyboard.event.state == true,
        cb_userdata->wl_data->keyboard.event.modifiers
    };

    wpe_view_backend_dispatch_keyboard_event (backend, &event);
}

static void
on_surface_frame (void *data, struct wl_callback *callback, uint32_t time)
{
    wl_callback_destroy (callback);
    struct wpe_view_backend_exportable_fdo *exportable = (struct wpe_view_backend_exportable_fdo*) data;
    wpe_view_backend_exportable_fdo_dispatch_frame_complete (exportable);
}

static const struct wl_callback_listener frame_listener = {
    .done = on_surface_frame
};

static void
request_frame (struct wpe_view_backend_exportable_fdo *exportable)
{
    struct wl_callback *frame_callback = wl_surface_frame (s_win_data->wl_surface);
    wl_callback_add_listener (frame_callback,
                              &frame_listener,
                              exportable);
}

static void
on_buffer_release (void* data, struct wl_buffer* buffer)
{
    WpeViewBackendData *backend_data = (WpeViewBackendData*) data;

    /* TODO: These asserts might be unnecessary, but having for now. */
    g_assert (backend_data);

    // Corner case: dispaching the last generated image. Required when the
    // active view changes.
    if (wpe_view_data.image && wpe_view_data.image != backend_data->image) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (backend_data->exportable, wpe_view_data.image);
    }
    wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image (backend_data->exportable, backend_data->image);
    g_clear_pointer (&buffer, wl_buffer_destroy);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = on_buffer_release,
};

static void
on_export_fdo_egl_image (void *data, struct wpe_fdo_egl_exported_image *image)
{
    WpeViewBackendData *backend_data = (WpeViewBackendData*) data;
    g_assert (backend_data);

    backend_data->image = image;
    wpe_view_data.image = image;
    wpe_view_backend_add_activity_state (backend_data->backend, wpe_view_activity_state_initiated);


    if (s_win_data->is_fullscreen) {
        struct wl_region *region;
        region = wl_compositor_create_region (s_pdisplay->compositor);
        wl_region_add (region, 0, 0, s_win_data->width, s_win_data->height);
        wl_surface_set_opaque_region (s_win_data->wl_surface, region);
        wl_region_destroy (region);
    } else {
        wl_surface_set_opaque_region (s_win_data->wl_surface, NULL);
    }

    static PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL
        s_eglCreateWaylandBufferFromImageWL;
    if (s_eglCreateWaylandBufferFromImageWL == NULL) {
        s_eglCreateWaylandBufferFromImageWL = (PFNEGLCREATEWAYLANDBUFFERFROMIMAGEWL)
            eglGetProcAddress ("eglCreateWaylandBufferFromImageWL");
        g_assert (s_eglCreateWaylandBufferFromImageWL);
    }

    struct wl_buffer *buffer = s_eglCreateWaylandBufferFromImageWL (s_pdisplay->egl_display, wpe_fdo_egl_exported_image_get_egl_image (image));
    g_assert (buffer);
    wl_buffer_add_listener (buffer, &buffer_listener, data);

    wl_surface_attach (s_win_data->wl_surface, buffer, 0, 0);
    wl_surface_damage (s_win_data->wl_surface,
                       0, 0,
                       s_win_data->width * wl_data.current_output.scale,
                       s_win_data->height * wl_data.current_output.scale);
    request_frame (backend_data->exportable);

    wl_surface_commit (s_win_data->wl_surface);
}

WebKitWebViewBackend*
cog_shell_new_fdo_view_backend (CogShell *shell)
{
    static struct wpe_view_backend_exportable_fdo_egl_client exportable_egl_client = {
        .export_fdo_egl_image = on_export_fdo_egl_image,
    };

    WpeViewBackendData *data = g_new0 (WpeViewBackendData, 1);

    data->exportable = wpe_view_backend_exportable_fdo_egl_create (&exportable_egl_client,
                                                                   data,
                                                                   DEFAULT_WIDTH,
                                                                   DEFAULT_HEIGHT);
    g_assert (data->exportable);

    /* init WPE view backend */
    data->backend =
        wpe_view_backend_exportable_fdo_get_view_backend (data->exportable);
    g_assert (data->backend);

    WebKitWebViewBackend *wk_view_backend =
        webkit_web_view_backend_new (data->backend,
                                     (GDestroyNotify) wpe_view_backend_data_free,
                                     data);
    g_assert (wk_view_backend);

    if (!wl_data.event_src) {
        wl_data.event_src =
            setup_wayland_event_source (g_main_context_get_thread_default (),
                                        s_pdisplay);
    }

    return wk_view_backend;
}


WebKitWebViewBackend*
cog_shell_fdo_resume_active_views (CogShell *shell)
{
    GSList* iterator = NULL;
    for (iterator = wpe_host_data_exportable; iterator; iterator =  iterator->next) {
        struct wpe_view_backend_exportable_fdo *exportable = iterator->data;
        struct wpe_view_backend *backend = wpe_view_backend_exportable_fdo_get_view_backend (exportable);
        if (wpe_view_backend_get_activity_state (backend) & wpe_view_activity_state_visible) {
            if (wpe_view_backend_get_activity_state (backend) & wpe_view_activity_state_initiated) {
                g_debug("cog_shell_resume_active_views: %p", exportable);
                request_frame (exportable);
            }
        }
    }
    WebKitWebView *webview = WEBKIT_WEB_VIEW(cog_shell_get_active_view (shell));
    return webkit_web_view_get_backend(webview);
}

static void
cog_fdo_shell_init (CogFdoShell *shell)
{
    TRACE ("");
}

gboolean
cog_fdo_shell_initable_init (GInitable *initable,
                             GCancellable *cancellable,
                             GError **error)
{
    if (cancellable != NULL) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Cancellable initialization not supported");
        return FALSE;
    }

    if (!wpe_loader_init ("libWPEBackend-fdo-1.0.so")) {
        g_debug ("%s: Could not initialize libwpe.", G_STRFUNC);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                             "Couldn't initialize the FDO backend.");
        return FALSE;
    }

    TRACE ("");
    s_win_data = g_new0 (PwlWinData, 1);
    s_callback_userdata = g_new0 (CogFdoShellCallbackData, 1);
    s_callback_userdata->shell = (CogShell*) initable;
    s_callback_userdata->wl_data = &wl_data;
    s_callback_userdata->win_data = s_win_data;

#if HAVE_DEVICE_SCALING
    const struct wl_output_listener output_listener = {
        .geometry = noop,
        .mode = noop,
        .done = noop,
        .scale = output_handle_scale,
    };
    wl_data.output_listener = output_listener;

    s_pdisplay->on_surface_enter = on_surface_enter;
    s_pdisplay->on_surface_enter_userdata = s_callback_userdata;
#endif /* HAVE_DEVICE_SCALING */
    s_pdisplay->on_pointer_on_motion = on_pointer_on_motion;
    s_pdisplay->on_pointer_on_motion_userdata = s_callback_userdata;
    s_pdisplay->on_pointer_on_button = on_pointer_on_button;
    s_pdisplay->on_pointer_on_button_userdata = s_callback_userdata;
    s_pdisplay->on_pointer_on_axis = on_pointer_on_axis;
    s_pdisplay->on_pointer_on_axis_userdata = s_callback_userdata;

    s_pdisplay->on_touch_on_down = on_touch_on_down;
    s_pdisplay->on_touch_on_down_userdata = s_callback_userdata;
    s_pdisplay->on_touch_on_up = on_touch_on_up;
    s_pdisplay->on_touch_on_up_userdata = s_callback_userdata;
    s_pdisplay->on_touch_on_motion = on_touch_on_motion;
    s_pdisplay->on_touch_on_motion_userdata = s_callback_userdata;

    s_pdisplay->on_key_event = on_key_event;
    s_pdisplay->on_key_event_userdata = s_callback_userdata;
    s_pdisplay->on_capture_app_key = on_capture_app_key;
    s_pdisplay->on_capture_app_key_userdata = s_callback_userdata;

    if (!init_wayland (s_pdisplay, error))
        return FALSE;

    if (!pwl_display_egl_init (s_pdisplay, error))
        return FALSE;

    if (!create_window (s_pdisplay, s_win_data, error)) {
        pwl_display_egl_deinit (s_pdisplay);
        return FALSE;
    }

    s_win_data->on_window_resize = on_window_resize;
    s_win_data->on_window_resize_userdata = s_callback_userdata;

    xkb_data.modifier.control = wpe_input_keyboard_modifier_control;
    xkb_data.modifier.alt = wpe_input_keyboard_modifier_alt;
    xkb_data.modifier.shift = wpe_input_keyboard_modifier_shift;

    if (!init_input (s_pdisplay, error)) {
        destroy_window (s_pdisplay, s_win_data);
        pwl_display_egl_deinit (s_pdisplay);
        return FALSE;
    }

    /* init WPE host data */
    wpe_fdo_initialize_for_egl_display (s_pdisplay->egl_display);

    return TRUE;
}

static void cog_fdo_shell_initable_iface_init (GInitableIface *iface)
{
    iface->init = cog_fdo_shell_initable_init;
}
