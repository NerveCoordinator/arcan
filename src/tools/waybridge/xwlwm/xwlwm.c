/*
 * Copyright 2016-2018, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: https://github.com/letoram/arcan/wiki/wayland.md
 * Description: XWayland specific 'wnddow Manager' that deals with the special
 * considerations needed for pairing XWayland redirected windows with wayland
 * surfaces etc. Decoupled from the normal XWayland so that both sides can be
 * sandboxed better and possibly used for a similar -rootless mode in Xarcan.
 *
 * Points:
 *  override_redirect : if set, don't focus window
 *
 */
#include <arcan_shmif.h>
#include <inttypes.h>
/* #include <X11/XCursor/XCursor.h> */
#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/xfixes.h>
#include <xcb/xcb_event.h>
#include <pthread.h>

static xcb_connection_t* dpy;
static xcb_screen_t* screen;
static xcb_drawable_t root;
static xcb_drawable_t wnd;
static xcb_colormap_t colormap;
static xcb_visualid_t visual;

#include "atoms.h"

/*
 * struct window, malloc, set id, HASH_ADD_INT(windows, id, new)
 * HASH_FIND_INT( users, &id, outptr)
 * HASH_DEL(windows, outptr)
 */

static inline void trace(const char* msg, ...)
{
	va_list args;
	va_start( args, msg );
		vfprintf(stderr,  msg, args );
		fprintf(stderr, "\n");
	va_end( args);
	fflush(stderr);
}

static void scan_atoms()
{
	for (size_t i = 0; i < ATOM_LAST; i++){
		xcb_intern_atom_cookie_t cookie =
			xcb_intern_atom(dpy, 0, strlen(atom_map[i]), atom_map[i]);

		xcb_generic_error_t* error;
		xcb_intern_atom_reply_t* reply =
			xcb_intern_atom_reply(dpy, cookie, &error);
		if (reply && !error){
			atoms[i] = reply->atom;
		}
		if (error){
			fprintf(stderr,
				"atom (%s) failed with code (%d)\n", atom_map[i], error->error_code);
			free(error);
		}
		free(reply);
	}

/* do we need to add xfixes here? */
}

static bool setup_visuals()
{
	xcb_depth_iterator_t depth =
		xcb_screen_allowed_depths_iterator(screen);

	while(depth.rem > 0){
		if (depth.data->depth == 32){
			visual = (xcb_depth_visuals_iterator(depth.data).data)->visual_id;
			colormap = xcb_generate_id(dpy);
			xcb_create_colormap(dpy, XCB_COLORMAP_ALLOC_NONE, colormap, root, visual);
			return true;
		}
		xcb_depth_next(&depth);
	}

	return false;
}

static void create_window()
{
	wnd = xcb_generate_id(dpy);
	xcb_create_window(dpy,
		XCB_COPY_FROM_PARENT, wnd, root,
		0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		visual, 0, NULL
	);
	xcb_change_property(dpy,
		XCB_PROP_MODE_REPLACE, wnd,
		atoms[NET_SUPPORTING_WM_CHECK], XCB_ATOM_WINDOW, 32, 1, &wnd);
/* wm name, utf8 string
 * supporting wm, selection_owner, ... */
}

static bool has_atom(
	xcb_get_property_reply_t* prop, enum atom_names atom)
{
	if (prop == NULL || xcb_get_property_value_length(prop) == 0)
		return false;

	xcb_atom_t* atom_query = xcb_get_property_value(prop);
	if (!atom_query){
		return false;
	}

	size_t count = xcb_get_property_value_length(prop) / (prop->format / 8);
	for (size_t i = 0; i < count; i++){
		if (atom_query[i] == atoms[atom]){
			return true;
		}
	}

	return false;
}

static void send_updated_window(
	const char* kind, uint32_t id, int32_t x, int32_t y)
{
/*
 * metainformation about the window to better select a type and behavior.
 *
 * _NET_WM_WINDOW_TYPE replaces MOTIF_WM_HINTS so we much prefer that as it
 * maps to the segment type.
 */
	xcb_get_property_cookie_t cookie = xcb_get_property(
		dpy, 0, id, atoms[NET_WM_WINDOW_TYPE], XCB_ATOM_ANY, 0, 2048);
	xcb_get_property_reply_t* reply = xcb_get_property_reply(dpy, cookie, NULL);

/* couldn't find out more, just map it and hope */
	bool popup = false, dnd = false, menu = false, notification = false;
	bool splash = false, tooltip = false, utility = false, dropdown = false;

	fprintf(stdout, "kind=%s:id=%"PRIu32, kind, id);
	if (reply){
		popup = has_atom(reply, NET_WM_WINDOW_TYPE_POPUP_MENU);
		dnd = has_atom(reply, NET_WM_WINDOW_TYPE_DND);
		dropdown = has_atom(reply, NET_WM_WINDOW_TYPE_DROPDOWN_MENU);
		menu  = has_atom(reply, NET_WM_WINDOW_TYPE_MENU);
		notification = has_atom(reply, NET_WM_WINDOW_TYPE_NOTIFICATION);
		splash = has_atom(reply, NET_WM_WINDOW_TYPE_SPLASH);
		tooltip = has_atom(reply, NET_WM_WINDOW_TYPE_TOOLTIP);
		utility = has_atom(reply, NET_WM_WINDOW_TYPE_UTILITY);
		free(reply);

		if (popup || dnd || dropdown || menu ||
			notification || splash || tooltip || utility){
			fprintf(stdout, ":type=%s", (popup || dropdown) ? "popup" : "subsurface");
		}
	}

	cookie = xcb_get_property(dpy,
		0, id, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 2048);
	reply = xcb_get_property_reply(dpy, cookie, NULL);

	if (reply){
		xcb_window_t* pwd = xcb_get_property_value(reply);
		fprintf(stdout, ":parent=%"PRIu32, *pwd);
		free(reply);
	}

/*
 * a bunch of translation heuristics here:
 *  transient_for ? convert to parent- relative coordinates unless input
 *  if input, set toplevel and viewport parent-
 *
 * do we have a map request coordinate?
 */

/*
 * WM_HINTS :
 *  flags as feature bitmap
 *  input, initial_state, pixmap, window, position, mask, group,
 *  message, urgency
 */
	fprintf(stdout, ":x=%"PRId32":y=%"PRId32"\n", x, y);
}

static void xcb_create_notify(xcb_create_notify_event_t* ev)
{
	send_updated_window("create", ev->window, ev->x, ev->y);
}

static void xcb_map_notify(xcb_map_notify_event_t* ev)
{
/* chances are that we get mapped with different atoms being set,
 * particularly for popups used by cutebrowser etc. */
	xcb_get_property_cookie_t cookie = xcb_get_property(dpy,
		0, ev->window, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 0, 2048);
	xcb_get_property_reply_t* reply = xcb_get_property_reply(dpy, cookie, NULL);

	if (reply){
		xcb_window_t* pwd = xcb_get_property_value(reply);
		fprintf(stdout,
			"kind=parent:id=%"PRIu32":parent_id=%"PRIu32"\n", ev->window, *pwd);
		free(reply);
	}
}

static void xcb_map_request(xcb_map_request_event_t* ev)
{
/* while the above could've made a round-trip to make sure we don't
 * race with the wayland channel, the approach of detecting surface-
 * type and checking seems to work ok (xwl.c) */
	xcb_map_window(dpy, ev->window);
	/*send_updated_window("map", ev->window); */
}

static void xcb_unmap_notify(xcb_unmap_notify_event_t* ev)
{
	fprintf(stdout, "kind=unmap:id=%"PRIu32"\n", ev->window);
}

static void xcb_client_message(xcb_client_message_event_t* ev)
{
/*
 * switch type against resolved atoms:
 * WL_SURFACE_ID : gives wayland surface id
 *  NET_WM_STATE : (format field == 32), gives:
 *                 modal, fullscreen, maximized_vert, maximized_horiz
 * NET_ACTIVE_WINDOW: set active window on root
 * NET_WM_MOVERESIZE: set edges for move-resize window
 * PROTOCOLS: set ping-pong
 */
	if (ev->type == atoms[WL_SURFACE_ID]){
		trace("wl-surface:%"PRIu32, ev->data.data32[0]);
		fprintf(stdout,
			"kind=surface:id=%"PRIu32":surface_id=%"PRIu32"\n",
			ev->window, ev->data.data32[0]
		);
	}
}

static void xcb_configure_notify(xcb_configure_notify_event_t* ev)
{
	trace("configure-notify:%"PRIu32" @%d,%d", ev->window, ev->x, ev->y);
	fprintf(stdout,
		"kind=configure:id=%"PRIu32":x=%d:y=%d:w=%d:h=%d\n",
		ev->window, ev->x, ev->y, ev->width, ev->height
	);
}

static void xcb_configure_request(xcb_configure_request_event_t* ev)
{
/* this needs to translate to _resize calls and to VIEWPORT hint events */
	fprintf(stdout,
		"kind=configure:id=%"PRIu32":x=%d:y=%d:w=%d:h=%d\n",
		ev->window, ev->x, ev->y, ev->width, ev->height
	);

/* just ack the configure request for now, this should really be deferred
 * until we receive the corresponding command from our parent but we lack
 * that setup right now */
	xcb_configure_window(dpy, ev->window,
		XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
		XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
		XCB_CONFIG_WINDOW_BORDER_WIDTH,
		(uint32_t[]){ev->x, ev->y, ev->width, ev->height, 0}
	);

/*
 * xcb_set_input_focus(dpy,
		XCB_INPUT_FOCUS_POINTER_ROOT, ev->window, XCB_CURRENT_TIME);
 */
}

/* use stdin/popen/line based format to make debugging easier */
static void process_wm_command(const char* arg)
{
	struct arg_arr* args = arg_unpack(arg);
	if (!args)
		return;

/* all commands have kind/id */
	const char* dst;
	if (!arg_lookup(args, "id", 0, &dst)){
		fprintf(stderr, "malformed argument: %s, missing id\n", arg);
	}

	uint32_t id = strtoul(dst, NULL, 10);
	if (!arg_lookup(args, "kind", 0, &dst)){
		fprintf(stderr, "malformed argument: %s, missing kind\n", arg);
		goto cleanup;
	}

/* match to previously known window so we get the right handle */

	if (strcmp(dst, "maximized") == 0){
		trace("srv-maximize");
	}
	else if (strcmp(dst, "fullscreen") == 0){
		trace("srv-fullscreen");
	}
	else if (strcmp(dst, "resize") == 0){
		arg_lookup(args, "width", 0, &dst);
		size_t w = strtoul(dst, NULL, 10);
		arg_lookup(args, "height", 0, &dst);
		size_t h = strtoul(dst, NULL, 10);
		trace("srv-resize(%d)(%zu, %zu)", id, w, h);

		xcb_configure_window(dpy, id,
			XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
			XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
			XCB_CONFIG_WINDOW_BORDER_WIDTH,
			(uint32_t[]){0, 0, w, h, 0}
		);
		xcb_flush(dpy);
	}
	else if (strcmp(dst, "destroy") == 0){
		trace("srv-destroy");
		xcb_destroy_window(dpy, id);
	}
	else if (strcmp(dst, "focus") == 0){
		trace("srv-focus");

		xcb_set_input_focus(dpy,
			XCB_INPUT_FOCUS_POINTER_ROOT, id, XCB_CURRENT_TIME);
	}

cleanup:
	arg_cleanup(args);
}

static void* process_thread(void* arg)
{
	while (!ferror(stdin) && !feof(stdin)){
		char inbuf[1024];
		if (fgets(inbuf, 1024, stdin))
			process_wm_command(inbuf);
	}
	return NULL;
}

int main (int argc, char **argv)
{
	int code;
	uint32_t values[3];

	xcb_generic_event_t *ev;

	if (getenv("ARCAN_XWLWM_DEBUGSTALL")){
		volatile bool sleeper = true;
		while (sleeper){}
	}

	if (!getenv("DISPLAY")){
		putenv("DISPLAY=:0");
	}

/* Missing: spawn XWayland in rootless mode */

	int counter = 10;
	while (counter--){
		dpy = xcb_connect(NULL, NULL);
		if ((code = xcb_connection_has_error(dpy))){
			fprintf(stderr, "Couldn't open display (%d)\n", code);
			sleep(1);
			continue;
		}
		break;
	}
	if (!counter)
		return EXIT_FAILURE;

	screen = xcb_setup_roots_iterator(xcb_get_setup(dpy)).data;
	root = screen->root;
	if (!setup_visuals()){
		fprintf(stderr, "Couldn't setup visuals/colormap\n");
		return EXIT_FAILURE;
	}

	scan_atoms();

/*
 * enable structure and redirection notifications so that we can forward
 * the related events onward to the active arcan window manager
 */
	values[0] =
		XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
		XCB_EVENT_MASK_PROPERTY_CHANGE;

	xcb_change_window_attributes(dpy, root, XCB_CW_EVENT_MASK, values);
	xcb_composite_redirect_subwindows(dpy, root, XCB_COMPOSITE_REDIRECT_MANUAL);
	xcb_flush(dpy);

	create_window();

/*
 * xcb is thread-safe, so we can have one thread for incoming
 * dispatch and another thread for outgoing dispatch
 */
	pthread_t pth;
	pthread_attr_t pthattr;
	pthread_attr_init(&pthattr);
	pthread_attr_setdetachstate(&pthattr, PTHREAD_CREATE_DETACHED);
	pthread_create(&pth, &pthattr, process_thread, NULL);

/* atom lookup:
 * moveresize, state, fullscreen, maximized vert, maximized horiz, active window
 */
	while( (ev = xcb_wait_for_event(dpy)) ){
		switch (ev->response_type & ~0x80) {
		case XCB_BUTTON_PRESS:
			trace("motion-notify");
		break;
		case XCB_MOTION_NOTIFY:
			trace("motion-notify");
		break;
		case XCB_BUTTON_RELEASE:
			trace("button-release");
		break;
		case XCB_ENTER_NOTIFY:
			trace("enter-notify");
		break;
		case XCB_CREATE_NOTIFY:
			xcb_create_notify((xcb_create_notify_event_t*) ev);
		break;
		case XCB_MAP_REQUEST:
			xcb_map_request((xcb_map_request_event_t*) ev);
		break;
    case XCB_MAP_NOTIFY:
			xcb_map_notify((xcb_map_notify_event_t*) ev);
		break;
    case XCB_UNMAP_NOTIFY:
			xcb_unmap_notify((xcb_unmap_notify_event_t*) ev);
		break;
    case XCB_REPARENT_NOTIFY:
			trace("reparent-notify");
		break;
    case XCB_CONFIGURE_REQUEST:
			xcb_configure_request((xcb_configure_request_event_t*) ev);
		break;
    case XCB_CONFIGURE_NOTIFY:
			xcb_configure_notify((xcb_configure_notify_event_t*) ev);
		break;
		case XCB_DESTROY_NOTIFY:{
			fprintf(stdout, "kind=destroy:id=%"PRIu32"\n",
				((xcb_destroy_notify_event_t*) ev)->window);
		}
		break;
		case XCB_MAPPING_NOTIFY:
			trace("mapping-notify");
		break;
		case XCB_PROPERTY_NOTIFY:
			trace("property-notify");
		break;
		case XCB_CLIENT_MESSAGE:
			trace("client-message");
			xcb_client_message((xcb_client_message_event_t*) ev);
		break;
		case XCB_FOCUS_IN:
			trace("focus-in");
		break;
		default:
			trace("unhandled");
		break;
		}
		xcb_flush(dpy);
		fflush(stdout);
	}

return 0;
}
