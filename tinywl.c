#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <pango/pangocairo.h>
#include <drm_fourcc.h>

/* For brevity's sake, struct members are annotated where they are used. */
enum tinywl_cursor_mode {
	TINYWL_CURSOR_PASSTHROUGH,
	TINYWL_CURSOR_MOVE,
	TINYWL_CURSOR_RESIZE,
	TINYWL_CURSOR_PRESSED,
};

struct tinywl_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_surface;
	struct wl_list views;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;
	enum tinywl_cursor_mode cursor_mode;
	struct tinywl_view *grabbed_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;
};

struct tinywl_output {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
};

struct previous_geo {
	int x, y, width, height;
};

struct title {
	struct wlr_scene_buffer *buffer;
	int original_width, current_width;
};

struct tinywl_view {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_xdg_surface *xdg_surface;
	struct wlr_scene_node *scene_node;
	struct wlr_scene_rect *decoration;
	struct title title;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener commit;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener set_title;
	struct previous_geo saved_geometry;
	int x, y;
};

struct tinywl_keyboard {
	struct wl_list link;
	struct tinywl_server *server;
	struct wlr_input_device *device;

	struct wl_listener modifiers;
	struct wl_listener key;
};

enum decoration_type {
	NONE,
	TITLEBAR,
	BORDER,
};

// These are not runtime configurable yet so make them 'const's
typedef struct Global_config {
	char *font_description;
	const int edge_margin;
	const int titlebar_padding;
	const int border_size;
	const int doubleclick_interval;
	const float active_window_rgba[4];
	const float inactive_window_rgba[4];
}Global_config;
const Global_config CONFIG = {
		"Sans 12", 2, 2, 3, 500,
		{ 0.0f, 0.47f, 0.8f, 1.0f },
		{ 0.33f, 0.33f, 0.33f, 1.0f }
};
int TITLEBAR_HEIGHT;

// From hopalong, is there a better way?
static struct tinywl_view *tinywl_view_from_wlr_surface(
		struct tinywl_server *server, struct wlr_surface *surface) {
	struct tinywl_view *view;

	wl_list_for_each(view, &server->views, link)
	{
		if (surface == view->xdg_surface->surface)
			return view;
	}

	return NULL;
}

static void focus_view(struct tinywl_view *view, struct wlr_surface *surface) {
	/* Note: this function only deals with keyboard focus. */
	if (view == NULL) {
		return;
	}
	struct tinywl_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	if (prev_surface == surface) {
		/* Don't re-focus an already focused surface. */
		return;
	}
	if (prev_surface) {
		/*
		 * Deactivate the previously focused surface. This lets the client know
		 * it no longer has focus and the client will repaint accordingly, e.g.
		 * stop displaying a caret.
		 */
		struct wlr_xdg_surface *previous = wlr_xdg_surface_from_wlr_surface(
					seat->keyboard_state.focused_surface);
		wlr_xdg_toplevel_set_activated(previous, false);

		/* Update the border to inactive color */
		struct tinywl_view *focused_view = tinywl_view_from_wlr_surface(
			server, prev_surface);
		if (focused_view && focused_view->decoration){
			wlr_scene_rect_set_color(focused_view->decoration, CONFIG.inactive_window_rgba);
		}
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	/* Move the view to the front */
	wlr_scene_node_raise_to_top(view->scene_node);
	wl_list_remove(&view->link);
	wl_list_insert(&server->views, &view->link);
	/* Activate the new surface */
	wlr_xdg_toplevel_set_activated(view->xdg_surface, true);
	/* Update the border to active color */
	if (view->decoration){
		wlr_scene_rect_set_color(view->decoration, CONFIG.active_window_rgba);
	}
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	wlr_seat_keyboard_notify_enter(seat, view->xdg_surface->surface,
		keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

static void save_view_geometry(struct tinywl_view *view){
	struct wlr_box view_geometry;
	wlr_xdg_surface_get_geometry(view->xdg_surface, &view_geometry);

	view->saved_geometry.x = view->x;
	view->saved_geometry.y = view->y;
	view->saved_geometry.height = view_geometry.height;
	view->saved_geometry.width = view_geometry.width;
}

bool maximize_view(struct tinywl_view *view, enum wlr_edges edge){
	// Return false if the view is already maximized
	if (view->xdg_surface->toplevel->current.maximized){
		return false;
	} else {
		/* Now that we can move from one maximized edge to another we don't want to
		 * save the state in that case and just continue to use the old geometry */
		if (view->server->cursor_mode != TINYWL_CURSOR_MOVE){
			save_view_geometry(view);
		};

        struct wlr_output *output =
            wlr_output_layout_output_at(view->server->output_layout,
		        view->server->cursor->x, view->server->cursor->y);
        if (!output){ return false; }

		int x, y, width, height;
		x = CONFIG.border_size;
		y = TITLEBAR_HEIGHT + CONFIG.border_size;
		width = output->width - CONFIG.border_size*2;
		height = output->height - (TITLEBAR_HEIGHT + CONFIG.border_size*2);

		switch (edge) {
		case WLR_EDGE_LEFT:
			width = output->width/2 - CONFIG.border_size*2;
			break;
		case WLR_EDGE_RIGHT:
			x = output->width/2 + CONFIG.border_size;
			width = output->width/2 - CONFIG.border_size*2;
			break;
		case WLR_EDGE_BOTTOM:
			y = output->height/2 + (TITLEBAR_HEIGHT + CONFIG.border_size);
			height = output->height/2 - (TITLEBAR_HEIGHT + CONFIG.border_size*2);
			break;
		case WLR_EDGE_TOP:
			height = output->height/2 - (TITLEBAR_HEIGHT + CONFIG.border_size*2);
			break;
		}

		view->x = x;
		view->y = y;
		wlr_scene_node_set_position(view->scene_node, view->x, view->y);
		wlr_xdg_toplevel_set_size(view->xdg_surface, width, height);
        wlr_xdg_toplevel_set_maximized(view->xdg_surface, true);
	}
	return true;
}

bool unmaximize_view(struct tinywl_view *view){
	// Return false if the view is not maximized
	if (view->xdg_surface->toplevel->current.maximized){
        view->x = view->saved_geometry.x;
        view->y = view->saved_geometry.y;
	    wlr_scene_node_set_position(view->scene_node, view->x, view->y);
        wlr_xdg_toplevel_set_size(view->xdg_surface, view->saved_geometry.width, view->saved_geometry.height);
        wlr_xdg_toplevel_set_maximized(view->xdg_surface, false);
    } else {
		return false;
	}
	return true;
}

void toggle_maximize(struct tinywl_view *view){
    if (!unmaximize_view(view))
		maximize_view(view, WLR_EDGE_NONE);
}

// Buffer logic from cagebreak
struct text_buffer {
	struct wlr_buffer base;
	void *data;
	uint32_t format;
	size_t stride;
};

static void text_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct text_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	free(buffer->data);
	free(buffer);
}

static bool text_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct text_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	if(data != NULL) {
		*data = (void *)buffer->data;
	}
	if(format != NULL) {
		*format = buffer->format;
	}
	if(stride != NULL) {
		*stride = buffer->stride;
	}
	return true;
}

static void text_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	// This space is intentionally left blank
}

static const struct wlr_buffer_impl text_buffer_impl = {
	.destroy = text_buffer_destroy,
	.begin_data_ptr_access = text_buffer_begin_data_ptr_access,
	.end_data_ptr_access = text_buffer_end_data_ptr_access,
};

static struct text_buffer *text_buffer_create(uint32_t width, uint32_t height, uint32_t stride) {
	struct text_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}

	wlr_buffer_init(&buffer->base, &text_buffer_impl, width, height);
	buffer->format = DRM_FORMAT_ARGB8888;
	buffer->stride = stride;

	buffer->data = malloc(buffer->stride * height);
	if (buffer->data == NULL) {
		free(buffer);
		return NULL;
	}

	return buffer;
}

static void get_text_size(char *text, char *font_str, int *width, int *height){
	cairo_surface_t *surface = cairo_image_surface_create(
			CAIRO_FORMAT_ARGB32, 0, 0);
	cairo_status_t status = cairo_surface_status(surface);

	cairo_t *cr = cairo_create(surface);
	PangoLayout *layout;
	PangoFontDescription *desc;

	/* Create Pango layout. */
	layout = pango_cairo_create_layout (cr);
	desc = pango_font_description_from_string (font_str);
	pango_layout_set_font_description (layout, desc);
	pango_font_description_free (desc);
	pango_layout_set_text (layout, text, -1);
	/* Set width and height to text size */
	pango_layout_get_pixel_size(layout, width, height);

	/* Cleanup */
	cairo_surface_destroy(surface);
	g_object_unref (layout);
	cairo_destroy(cr);

}

static struct text_buffer * create_text_buffer(struct tinywl_view *view,
		char* text) {
	int width, height;
	get_text_size(text, CONFIG.font_description, &width, &height);
	TITLEBAR_HEIGHT = height + CONFIG.titlebar_padding * 2;
	view->title.original_width = width;

	int pending_width =
		view->xdg_surface->surface->current.width - CONFIG.border_size;
	if (pending_width > 0 && width > pending_width)
		width = pending_width;
	view->title.current_width = width;

	cairo_surface_t *surface = cairo_image_surface_create(
			CAIRO_FORMAT_ARGB32, width, height);
	cairo_status_t status = cairo_surface_status(surface);
	if (status != CAIRO_STATUS_SUCCESS) {
		wlr_log(WLR_ERROR, "cairo_image_surface_create failed: %s",
			cairo_status_to_string(status));
		return NULL;
	}

	cairo_t *cr = cairo_create(surface);
	PangoLayout *layout;
	PangoFontDescription *desc;

	/* Set background to be transparent */
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint (cr);

	/* Create Pango layout. */
	layout = pango_cairo_create_layout (cr);
	desc = pango_font_description_from_string (CONFIG.font_description);
	pango_layout_set_font_description (layout, desc);
	pango_font_description_free (desc);
	pango_layout_set_text (layout, text, -1);
	pango_layout_set_width (layout, width * PANGO_SCALE);
	pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_MIDDLE);

	/* Draw layout. */
	cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
	pango_cairo_show_layout (cr, layout);

	//---
	unsigned char *data = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	struct text_buffer *buf=text_buffer_create(width, height, stride);
	void *data_ptr;

	if(!wlr_buffer_begin_data_ptr_access(&buf->base, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
			&data_ptr, NULL, NULL)) {
		wlr_log(WLR_ERROR, "Failed to get pointer access to text buffer");
		return NULL;
	}
	memcpy(data_ptr, data, stride*height);
	wlr_buffer_end_data_ptr_access(&buf->base);
	cairo_surface_destroy(surface);
	//-----

	/* free the layout object */
	g_object_unref (layout);
	cairo_destroy(cr);
	return buf;
}

static void view_title_update(struct tinywl_view *view,
		char* title_str){
	if (view->title.buffer)
		wlr_scene_node_destroy(&view->title.buffer->node);

	struct text_buffer *buf = create_text_buffer(view, title_str);
	struct wlr_scene_buffer *text_scene_buffer = malloc(sizeof(struct wlr_scene_buffer));
	view->title.buffer = wlr_scene_buffer_create(view->scene_node, &buf->base);

	wlr_scene_node_set_position(&view->title.buffer->node,
		CONFIG.titlebar_padding,
		CONFIG.titlebar_padding - TITLEBAR_HEIGHT);
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data){
	struct tinywl_view *view = wl_container_of(listener, view, set_title);
    view_title_update(view, view->xdg_surface->toplevel->title);
}

static void position_view_centered(struct tinywl_view *view){
	int main_width, main_height;
    if (view->xdg_surface->toplevel->parent){
        struct wlr_box geo_box;
        wlr_xdg_surface_get_geometry(view->xdg_surface->toplevel->parent, &geo_box);
        main_width = geo_box.width;
        main_height = geo_box.height;
    } else {
        struct wlr_output *output =
        wlr_output_layout_output_at(view->server->output_layout,
            view->server->cursor->x, view->server->cursor->y);
        main_width = output->width;
        main_height = output->height;
    }

    if (main_width){
		struct wlr_box view_geometry;
		wlr_xdg_surface_get_geometry(view->xdg_surface, &view_geometry);
        view->x = main_width/2 - view_geometry.width/2;
        view->y = main_height/2 - view_geometry.height/2;
        wlr_scene_node_set_position(view->scene_node, view->x, view->y);
    };
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->device);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->device->keyboard->modifiers);
}

static bool handle_keybinding(struct tinywl_server *server, xkb_keysym_t sym) {
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 *
	 * This function assumes Alt is held down.
	 */
	switch (sym) {
	case XKB_KEY_Escape:
		wl_display_terminate(server->wl_display);
		break;
	case XKB_KEY_F1:
		/* Cycle to the next view */
		if (wl_list_length(&server->views) < 2) {
			break;
		}
		struct tinywl_view *next_view = wl_container_of(
			server->views.prev, next_view, link);
		focus_view(next_view, next_view->xdg_surface->surface);
		break;
	default:
		return false;
	}
	return true;
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a key is pressed or released. */
	struct tinywl_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct tinywl_server *server = keyboard->server;
	struct wlr_event_keyboard_key *event = data;
	struct wlr_seat *seat = server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->device->keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->device->keyboard);
	if ((modifiers & WLR_MODIFIER_ALT) &&
			event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* If alt is held down and this button was _pressed_, we attempt to
		 * process it as a compositor keybinding. */
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(server, syms[i]);
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->device);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void server_new_keyboard(struct tinywl_server *server,
		struct wlr_input_device *device) {
	struct tinywl_keyboard *keyboard =
		calloc(1, sizeof(struct tinywl_keyboard));
	keyboard->server = server;
	keyboard->device = device;

	/* We need to prepare an XKB keymap and assign it to the keyboard. This
	 * assumes the defaults (e.g. layout = "us"). */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	wlr_keyboard_set_keymap(device->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(device->keyboard, 25, 600);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&device->keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&device->keyboard->events.key, &keyboard->key);

	wlr_seat_set_keyboard(server->seat, device);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct tinywl_server *server,
		struct wlr_input_device *device) {
	/* We don't do anything special with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In TinyWL we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct tinywl_server *server = wl_container_of(
			listener, server, request_cursor);
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	/* This can be sent by any client, so we check to make sure this one is
	 * actually has pointer focus first. */
	if (focused_client == event->seat_client) {
		/* Once we've vetted the client, we can tell the cursor to use the
		 * provided surface as the cursor image. It will set the hardware cursor
		 * on the output that it's currently on and continue to do so as the
		 * cursor moves between outputs. */
		wlr_cursor_set_surface(server->cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in tinywl we always honor
	 */
	struct tinywl_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static struct tinywl_view *desktop_view_at(
		struct tinywl_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy,
		enum decoration_type *decoration_type) {
	/* This returns the topmost node in the scene at the given layout coords. */
	struct wlr_scene_node *node, *topmost_node;
	node = topmost_node = wlr_scene_node_at(
		&server->scene->node, lx, ly, sx, sy);
	if (node == NULL) {
		return NULL;
	}
	/* Find the node corresponding to the tinywl_view at the root of this
	 * surface tree, it is the only one for which we set the data field. */
	while (topmost_node != NULL && topmost_node->data == NULL) {
		topmost_node = topmost_node->parent;
	}
	struct tinywl_view *view = topmost_node->data;
	*surface = view->xdg_surface->surface;

	if (node->type == WLR_SCENE_NODE_RECT){
		struct wlr_scene_rect *rect = (struct wlr_scene_rect *)node;
		if (rect != view->decoration)
			return NULL;

		if (*sx <= CONFIG.border_size || *sy <= CONFIG.border_size ||
				*sx >= rect->width - CONFIG.border_size ||
				*sy >= rect->height - CONFIG.border_size)
			*decoration_type = BORDER;
		else
			*decoration_type = TITLEBAR;
	} else if (node->type == WLR_SCENE_NODE_BUFFER){
		if ((struct wlr_scene_buffer *)node != view->title.buffer)
			return NULL;

		*decoration_type = TITLEBAR;
	}
	return topmost_node->data;
}

static void process_cursor_move(struct tinywl_server *server, uint32_t time) {
	struct tinywl_view *view = server->grabbed_view;
	struct wlr_output *output =
		wlr_output_layout_output_at(view->server->output_layout,
			view->server->cursor->x, view->server->cursor->y);
	if (!output){ return; }

	if (server->cursor->x <= CONFIG.edge_margin)
		maximize_view(view, WLR_EDGE_LEFT);
	else if (server->cursor->x >= output->width - CONFIG.edge_margin)
		maximize_view(view, WLR_EDGE_RIGHT);
	else if (server->cursor->y >= output->height - CONFIG.edge_margin)
		maximize_view(view, WLR_EDGE_BOTTOM);
	else if (server->cursor->y <= CONFIG.edge_margin)
		maximize_view(view, WLR_EDGE_TOP);
	else{
		// Unmaximize the window if it is maximized.
		unmaximize_view(view);

		/* Move the grabbed view to the new position. */
		view->x = server->cursor->x - server->grab_x;
		view->y = server->cursor->y - server->grab_y;
		wlr_scene_node_set_position(view->scene_node, view->x, view->y);
	}
}

static void process_cursor_resize(struct tinywl_server *server, uint32_t time) {
	/*
	 * Resizing the grabbed view can be a little bit complicated, because we
	 * could be resizing from any corner or edge. This not only resizes the view
	 * on one or two axes, but can also move the view if you resize from the top
	 * or left edges (or top-left corner).
	 *
	 * Note that I took some shortcuts here. In a more fleshed-out compositor,
	 * you'd wait for the client to prepare a buffer at the new size, then
	 * commit any movement that was prepared.
	 */
	struct tinywl_view *view = server->grabbed_view;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box geo_box;
	wlr_xdg_surface_get_geometry(view->xdg_surface, &geo_box);
	view->x = new_left - geo_box.x;
	view->y = new_top - geo_box.y;
	wlr_scene_node_set_position(view->scene_node, view->x, view->y);

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;
	wlr_xdg_toplevel_set_size(view->xdg_surface, new_width, new_height);
}

enum wlr_edges find_resize_edge(struct tinywl_view *view,
		struct wlr_surface *surface) {
    enum wlr_edges edge = 0;
    if (view->server->cursor->x < view->x + CONFIG.border_size) {
        edge |= WLR_EDGE_LEFT;
    }
    if (view->server->cursor->y < view->y + CONFIG.border_size) {
        edge |= WLR_EDGE_TOP;
    }
    if (view->server->cursor->x >=
            view->x + surface->pending.width - CONFIG.border_size) {
        edge |= WLR_EDGE_RIGHT;
    }
    if (view->server->cursor->y >=
            view->y + surface->pending.height - CONFIG.border_size) {
        edge |= WLR_EDGE_BOTTOM;
    }
    return edge;
}

// Forward declare, alternatively this function could be moved here.
static void begin_interactive(struct tinywl_view *view,
		enum tinywl_cursor_mode mode, uint32_t edges);

static void process_cursor_motion(struct tinywl_server *server, uint32_t time) {
	/* If the mode is non-passthrough, delegate to those functions. */
	if (server->cursor_mode == TINYWL_CURSOR_MOVE) {
		process_cursor_move(server, time);
		return;
	} else if (server->cursor_mode == TINYWL_CURSOR_RESIZE) {
		process_cursor_resize(server, time);
		return;
	}

	/* Otherwise, find the view under the pointer and send the event along. */
	double sx, sy;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = NULL;
	enum decoration_type decoration_type;
	struct tinywl_view *view = desktop_view_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy, &decoration_type);

	if ((!view || decoration_type == TITLEBAR) && server->cursor_mode != TINYWL_CURSOR_PRESSED) {
		/* If there's no view under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any views. */
		wlr_xcursor_manager_set_cursor_image(
				server->cursor_mgr, "left_ptr", server->cursor);
	} else if(decoration_type == TITLEBAR && server->cursor_mode == TINYWL_CURSOR_PRESSED){
        wlr_xcursor_manager_set_cursor_image(
            server->cursor_mgr, "move", server->cursor);
        server->seat->pointer_state.focused_surface = surface;
        begin_interactive(view, TINYWL_CURSOR_MOVE, 0);
    }else if (decoration_type == BORDER){
        enum wlr_edges edge = find_resize_edge(view, surface);
        wlr_xcursor_manager_set_cursor_image(
            server->cursor_mgr, wlr_xcursor_get_resize_name(edge), server->cursor);
    }

	if (server->cursor_mode == TINYWL_CURSOR_PRESSED && view != server->grabbed_view) {
        // Send pointer events to the view which the mouse button is pressed on.
		view = server->grabbed_view;
        sx = server->cursor->x - view->x;
		sy = server->cursor->y - view->y;
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
	} else if (surface) {
		/*
		 * Send pointer enter and motion events.
		 *
		 * The enter event gives the surface "pointer focus", which is distinct
		 * from keyboard focus. You get pointer focus by moving the pointer over
		 * a window.
		 *
		 * Note that wlroots will avoid sending duplicate enter/motion events if
		 * the surface has already has pointer focus or if the client is already
		 * aware of the coordinates passed.
		 */
		if (decoration_type == NONE){
            wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
            wlr_seat_pointer_notify_motion(seat, time, sx, sy);
        } else if (decoration_type != NONE && server->cursor_mode != TINYWL_CURSOR_PRESSED){
            wlr_seat_pointer_clear_focus(seat);
        }
	} else {
		/* Clear pointer focus so future button events and such are not sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_event_pointer_motion *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	wlr_cursor_move(server->cursor, event->device,
			event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. There is also some hardware which
	 * emits these events. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event = data;
	wlr_cursor_warp_absolute(server->cursor, event->device, event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static int number_of_clicks(uint32_t button, uint32_t time_msec){
    // Count number of clicks in a doubleclick_interval
    static int clicked = 1;
    static uint32_t last_button;
    static uint32_t last_time_pressed;

    if (button == last_button &&
			(time_msec - last_time_pressed) < CONFIG.doubleclick_interval){
        clicked++;
    } else {
        clicked = 1;
    }

    last_button = button;
    last_time_pressed = time_msec;
    return clicked;
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits a button
	 * event. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_event_pointer_button *event = data;
	/* Notify the client with pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
	double sx, sy;
	struct wlr_surface *surface = NULL;
	enum decoration_type decoration_type;
	struct tinywl_view *view = desktop_view_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy, &decoration_type);
	if (event->state == WLR_BUTTON_RELEASED) {
		/* If you released any buttons, we exit interactive move/resize mode. */
		server->cursor_mode = TINYWL_CURSOR_PASSTHROUGH;

		// Handle button events on titlebar portion
		int clicked = number_of_clicks(event->button, event->time_msec);
        if (decoration_type == TITLEBAR){
            if (event->button == BTN_LEFT && clicked == 2){
                toggle_maximize(view);
            } else if (event->button == BTN_MIDDLE){
                wlr_xdg_toplevel_send_close(view->xdg_surface);
            }
        }

		// The view might have changed (maximized) thus simulate move to update cursor
        process_cursor_motion(server, event->time_msec);
	} else {
		/* Focus that client if the button was _pressed_ */
		focus_view(view, surface);
		if (view){
			server->grabbed_view = view;
			if (decoration_type == BORDER){
				/* If we are clicking the border, then the surface is pointer focus is
			 	 * cleared and we need to manually set the focused surface without
				 * calling an enter, which would change the cursor image. */
                server->seat->pointer_state.focused_surface = surface;
                begin_interactive(view, TINYWL_CURSOR_RESIZE,
					find_resize_edge(view, surface));
            } else {
                server->cursor_mode = TINYWL_CURSOR_PRESSED;
            }
		}
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_event_pointer_axis *event = data;
	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	struct tinywl_server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

static void output_frame(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct tinywl_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);

	/* Render the scene if needed and commit the output */
	wlr_scene_output_commit(scene_output);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Configures the output created by the backend to use our allocator
	 * and our renderer. Must be done once, before commiting the output */
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	/* Some backends don't have modes. DRM+KMS does, and we need to set a mode
	 * before we can use the output. The mode is a tuple of (width, height,
	 * refresh rate), and each monitor supports only a specific set of modes. We
	 * just pick the monitor's preferred mode, a more sophisticated compositor
	 * would let the user configure it. */
	if (!wl_list_empty(&wlr_output->modes)) {
		struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
		wlr_output_set_mode(wlr_output, mode);
		wlr_output_enable(wlr_output, true);
		if (!wlr_output_commit(wlr_output)) {
			return;
		}
	}

	/* Allocates and configures our state for this output */
	struct tinywl_output *output =
		calloc(1, sizeof(struct tinywl_output));
	output->wlr_output = wlr_output;
	output->server = server;
	/* Sets up a listener for the frame notify event. */
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	wl_list_insert(&server->outputs, &output->link);

	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	wlr_output_layout_add_auto(server->output_layout, wlr_output);
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct tinywl_view *view = wl_container_of(listener, view, map);

	position_view_centered(view);

	wl_list_insert(&view->server->views, &view->link);

	focus_view(view, view->xdg_surface->surface);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct tinywl_view *view = wl_container_of(listener, view, unmap);

	wl_list_remove(&view->link);

	// Destroy commit listener and node for decorations
	wl_list_remove(&view->commit.link);
	wlr_scene_node_destroy(view->scene_node);
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	/* Called when the surface is destroyed and should never be shown again. */
	struct tinywl_view *view = wl_container_of(listener, view, destroy);

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->set_title.link);

	free(view);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct tinywl_view *view = wl_container_of(listener, view, commit);

	int pending_width = view->xdg_surface->pending.geometry.width;
	int pending_height = view->xdg_surface->pending.geometry.height;

	// Only render a new title if the width of the view is different than title
	if (pending_width < view->title.current_width ||
			(view->title.current_width != view->title.original_width &&
			view->title.current_width != pending_width - CONFIG.border_size)){
		view_title_update(view, view->xdg_surface->toplevel->title);
	}

    // This needs to be done here otherwise the border/titlebar move faster/slower
    // than the surface when the size is changed thus causing a lag effect.
    if (view->decoration && (pending_width != view->decoration->width ||
            pending_height != view->decoration->height - TITLEBAR_HEIGHT - CONFIG.border_size)){
        wlr_scene_rect_set_size(view->decoration, pending_width + (CONFIG.border_size*2),
                pending_height + TITLEBAR_HEIGHT + (CONFIG.border_size*2));
    }
}

/* This function is from labwc that calulates the view/window
 * position under the mouse proportional to when it was maximized. */
static int max_move_scale(double pos_cursor, double pos_current,
	double size_current, double size_orig)
{
	double anchor_frac = (pos_cursor - pos_current) / size_current;
	int pos_new = pos_cursor - (size_orig * anchor_frac);
	if (pos_new < pos_current) {
		/* Clamp by using the old offsets of the maximized window */
		pos_new = pos_current;
	}
	return pos_new;
}

static void begin_interactive(struct tinywl_view *view,
		enum tinywl_cursor_mode mode, uint32_t edges) {
	/* This function sets up an interactive move or resize operation, where the
	 * compositor stops propegating pointer events to clients and instead
	 * consumes them itself, to move or resize windows. */
	struct tinywl_server *server = view->server;
	struct wlr_surface *focused_surface =
		server->seat->pointer_state.focused_surface;
	if (view->xdg_surface->surface !=
			wlr_surface_get_root_surface(focused_surface)) {
		/* Deny move/resize requests from unfocused clients. */
		return;
	}
	server->grabbed_view = view;
	server->cursor_mode = mode;

	if (mode == TINYWL_CURSOR_MOVE) {
		if (view->xdg_surface->toplevel->current.maximized){
			// Calculate where the window should be under the cursor
			int new_x = max_move_scale(server->cursor->x, view->x,
				focused_surface->pending.width, view->saved_geometry.width);
            int new_y = max_move_scale(server->cursor->y, view->y,
				focused_surface->pending.height, view->saved_geometry.height);
			view->x = new_x;
            view->y = new_y;
		} else {
			// Save geometry before start of a move since we can maxmize with a move
			save_view_geometry(view);
		}
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
	} else {
		struct wlr_box geo_box;
		wlr_xdg_surface_get_geometry(view->xdg_surface, &geo_box);

		double border_x = (view->x + geo_box.x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y = (view->y + geo_box.y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = geo_box;
		server->grab_geobox.x += view->x;
		server->grab_geobox.y += view->y;

		server->resize_edges = edges;
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct tinywl_view *view = wl_container_of(listener, view, request_move);
	begin_interactive(view, TINYWL_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct tinywl_view *view = wl_container_of(listener, view, request_resize);
	begin_interactive(view, TINYWL_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data){
	struct tinywl_view *view = wl_container_of(listener, view, request_maximize);
    toggle_maximize(view);
}

static void server_new_xdg_surface(struct wl_listener *listener, void *data) {
	/* This event is raised when wlr_xdg_shell receives a new xdg surface from a
	 * client, either a toplevel (application window) or popup. */
	struct tinywl_server *server =
		wl_container_of(listener, server, new_xdg_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	/* We must add xdg popups to the scene graph so they get rendered. The
	 * wlroots scene graph provides a helper for this, but to use it we must
	 * provide the proper parent scene node of the xdg popup. To enable this,
	 * we always set the user data field of xdg_surfaces to the corresponding
	 * scene node. */
	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		struct wlr_xdg_surface *parent = wlr_xdg_surface_from_wlr_surface(
			xdg_surface->popup->parent);
		struct wlr_scene_node *parent_node = parent->data;
		xdg_surface->data = wlr_scene_xdg_surface_create(
			parent_node, xdg_surface);
		return;
	}
	assert(xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL);

	/* Allocate a tinywl_view for this surface */
	struct tinywl_view *view =
		calloc(1, sizeof(struct tinywl_view));
	view->server = server;
	view->xdg_surface = xdg_surface;
	/* If the new surface has a parent create it as part of the parent. Doing
	 * this will ensure that a dialog will be seen when it's parent is focused.*/
    if (xdg_surface->toplevel->parent != 0) {
        view->scene_node = wlr_scene_xdg_surface_create(
			xdg_surface->toplevel->parent->data, view->xdg_surface);
    } else {
        view->scene_node = &wlr_scene_tree_create(&view->server->scene->node)->node;
		view_title_update(view, view->xdg_surface->toplevel->title);
		view->decoration = wlr_scene_rect_create(
			view->scene_node, 0, 0, CONFIG.inactive_window_rgba);
        wlr_scene_xdg_surface_create(view->scene_node, view->xdg_surface);
        // Set the decoration position. The size is handled by the commit handler
        wlr_scene_node_set_position(&view->decoration->node, -CONFIG.border_size,
			-(TITLEBAR_HEIGHT + CONFIG.border_size));
    }
	view->scene_node->data = view;
	xdg_surface->data = view->scene_node;

	/* Listen to the various events it can emit */
	view->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_surface->events.map, &view->map);
	view->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &view->unmap);
	view->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
	view->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);

	/* cotd */
	struct wlr_xdg_toplevel *toplevel = xdg_surface->toplevel;
	view->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&toplevel->events.request_resize, &view->request_resize);
	view->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
	view->set_title.notify = xdg_toplevel_set_title;
	wl_signal_add(&toplevel->events.set_title, &view->set_title);
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	struct tinywl_server server;

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	server.wl_display = wl_display_create();
	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	server.backend = wlr_backend_autocreate(server.wl_display);

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	server.renderer = wlr_renderer_autocreate(server.backend);
	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	 server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	wlr_compositor_create(server.wl_display, server.renderer);
	wlr_data_device_manager_create(server.wl_display);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	server.output_layout = wlr_output_layout_create();

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	/* Create a scene graph. This is a wlroots abstraction that handles all
	 * rendering and damage tracking. All the compositor author needs to do
	 * is add things that should be rendered to the scene graph at the proper
	 * positions and then call wlr_scene_output_commit() to render a frame if
	 * necessary.
	 */
	server.scene = wlr_scene_create();
	wlr_scene_attach_output_layout(server.scene, server.output_layout);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
			wlr_server_decoration_manager_create(server.wl_display),
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	wlr_xdg_decoration_manager_v1_create(server.wl_display);

	/* Set up the xdg-shell. The xdg-shell is a Wayland protocol which is used
	 * for application windows. For more detail on shells, refer to my article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&server.views);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display);
	server.new_xdg_surface.notify = server_new_xdg_surface;
	wl_signal_add(&server.xdg_shell->events.new_surface,
			&server.new_xdg_surface);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). We add a cursor theme at scale factor 1 to begin with. */
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	wlr_xcursor_manager_load(server.cursor_mgr, 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in my
	 * input handling blog post:
	 *
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);

	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", socket, true);
	if (startup_cmd) {
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
		}
	}
	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			socket);
	wl_display_run(server.wl_display);

	/* Once wl_display_run returns, we shut down the server. */
	wl_display_destroy_clients(server.wl_display);
	wl_display_destroy(server.wl_display);
	return 0;
}
