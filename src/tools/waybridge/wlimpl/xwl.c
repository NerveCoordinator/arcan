/*
 * There are a number of oddities with dealing with XWayland, and
 * its behavior change depending on if you are using rootless mode
 * or not.
 *
 * With 'normal' mode it behaves as a dumb (and buggy) wl_shell
 * client that basically ignored everything.
 *
 * With 'rootless' mode, it creates compositor surfaces and uses
 * them directly - being basically the only client to do so. The
 * job then is to pair these surfaces based on a window property
 * and just treat them as something altogether special by adding
 * a custom window-manager.
 *
 * The process we're using is that whenever a compositor surface
 * tries to commit, we check if we are running with xwayland and
 * then fires up a window manager whi
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

static FILE* wmfd_output = NULL;
static pid_t xwl_wm_pid = -1;

static int wmfd_input = -1;
static char wmfd_inbuf[256];
static size_t wmfd_ofs = 0;

/* "known" mapped windows, we trigger the search etc. when a buffer
 * is commited without a known backing on the compositor, and try
 * to 'pair' it with ones that we have been told about */
struct xwl_window {

/* Xid for Window */
	uint32_t id;

/* Wayland client-local reference ID */
	uint32_t surface_id;

/* Parent Xid for Window */
	uint32_t parent_id;

/* Resolved 'arcan' type */
	int segid;

/* Track viewport separate from comp_surf viewport as it can be
 * populated when there is still no surf to pair it to */
	struct arcan_event viewport;

/*
 * Pairing approach is flawed, we need to defer the commit- release
 * stage on mismatch, not just assume and alloc as that'll just
 * break stuff.
 */
	bool paired;
	struct comp_surf* surf;
};

/* just linear search in a fixed buffer for now, scaling problems
 * are elsewhere for quite some time to come

#include "../uthash.h"
struct window {
	uint32_t id;
	xcb_window_t wnd;
	UT_hash_handle hh;
};
static struct window* windows;
*/

static struct xwl_window xwl_windows[256];
static struct xwl_window* xwl_find(uint32_t id)
{
	for (size_t i = 0; i < COUNT_OF(xwl_windows); i++)
		if (xwl_windows[i].id == id)
			return &xwl_windows[i];

	return NULL;
}

static struct xwl_window* xwl_find_surface(uint32_t id)
{
	for (size_t i = 0; i < COUNT_OF(xwl_windows); i++)
		if (xwl_windows[i].surface_id == id)
			return &xwl_windows[i];

	return NULL;
}

static struct xwl_window* xwl_find_alloc(uint32_t id)
{
	struct xwl_window* wnd = xwl_find(id);
	if (wnd)
		return wnd;

/* default to 'toplevel' like behavior */
	wnd = xwl_find(0);
	wnd->segid = SEGID_APPLICATION;
	return wnd;
}

static void wnd_viewport(struct xwl_window* wnd)
{
	if (!wnd->surf)
		return;

/* always re-resolve parent token */
	wnd->viewport.ext.viewport.parent = 0;
	if (wnd->parent_id > 0){
		struct xwl_window* pwnd = xwl_find(wnd->parent_id);
		if (!pwnd || !pwnd->surf){
			trace(TRACE_XWL, "bad parent id:%"PRIu32, wnd->parent_id);
		}
		else{
			wnd->viewport.ext.viewport.parent = pwnd->surf->acon.segment_token;
		}
	}

	arcan_shmif_enqueue(&wnd->surf->acon, &wnd->viewport);

	trace(TRACE_XWL, "viewport id:%"PRIu32",parent:%"PRIu32"@%"PRId32",%"PRId32,
		wnd->id, wnd->parent_id,
		wnd->viewport.ext.viewport.x, wnd->viewport.ext.viewport.y
	);
}

/*
 * Take an input- line from the window manager, unpack it, and interpret the
 * command inside. A noticable part here is that the resolved window may be
 * in an unallocated, unpaired or paired state here and the input itself is
 * not necessarily trusted.
 *
 * Thus any extracted field or update that should propagate as an event to
 * a backing shmif connection need to be support being deferred until
 * pairing / allocation - and resist UAF/spoofing. Luckily, there is not
 * many events that need to be forwarded.
 */
static void process_input(const char* msg)
{
	trace(TRACE_XWL, "wm->%s", msg);
	struct arg_arr* cmd = arg_unpack(msg);
	if (!cmd){
		trace(TRACE_XWL, "malformed message: %s", msg);
		return;
	}

/* all commands should have a 'kind' field */
	const char* arg;
	if (!arg_lookup(cmd, "kind", 0, &arg)){
		trace(TRACE_XWL, "malformed argument: %s, missing kind", msg);
		goto cleanup;
	}
/* pair surface */
	else if (strcmp(arg, "surface") == 0){
		if (!arg_lookup(cmd, "id", 0, &arg)){
			trace(TRACE_XWL, "malformed surface argument: missing id");
			goto cleanup;
		}
		uint32_t id = strtoul(arg, NULL, 10);
		if (!arg_lookup(cmd, "surface_id", 0, &arg)){
			trace(TRACE_XWL, "malformed surface argument: missing surface id");
			goto cleanup;
		}
		uint32_t surface_id = strtoul(arg, NULL, 10);
		trace(TRACE_XWL, "surface id:%"PRIu32"-%"PRIu32, id, surface_id);
		struct xwl_window* wnd = xwl_find_alloc(id);
		if (!wnd)
			goto cleanup;
		wnd->surface_id = surface_id;
		wnd->id = id;
		wnd->paired = true;
	}
/* window goes from invisible to visible state */
	else if (strcmp(arg, "create") == 0){
		if (!arg_lookup(cmd, "id", 0, &arg)){
			trace(TRACE_XWL, "malformed map argument: missing id");
			goto cleanup;
		}
		uint32_t id = strtoul(arg, NULL, 10);
		trace(TRACE_XWL, "map id:%"PRIu32, id);
		struct xwl_window* wnd = xwl_find_alloc(id);
		if (!wnd)
			goto cleanup;

		wnd->id = id;
		if (arg_lookup(cmd, "type", 0, &arg)){
			trace(TRACE_XWL, "mapped with type %s", arg);
			if (strcmp(arg, "popup") == 0){
				wnd->viewport.ext.viewport.focus = true;
				wnd->segid = SEGID_POPUP;
			}
			else
				wnd->segid = SEGID_MEDIA;
		}
		else
			wnd->segid = SEGID_APPLICATION;

		if (arg_lookup(cmd, "parent", 0, &arg)){
			uint32_t parent_id = strtoul(arg, NULL, 0);
			struct xwl_window* wnd = xwl_find(parent_id);
			if (wnd)
				wnd->parent_id = parent_id;
			else
				trace(TRACE_XWL, "bad parent-id: "PRIu32, parent_id);
		}
	}
	else if (strcmp(arg, "parent") == 0){
		if (!arg_lookup(cmd, "id", 0, &arg))
			goto cleanup;
		uint32_t id = strtoul(arg, NULL, 10);

		if (!arg_lookup(cmd, "parent_id", 0, &arg))
			goto cleanup;
		uint32_t parent_id = strtoul(arg, NULL, 10);
		struct xwl_window* wnd = xwl_find(id);
		if (!wnd)
			goto cleanup;
		wnd->parent_id = parent_id;
		trace(TRACE_XWL, "reparent id:%"PRIu32" to %"PRIu32, id, parent_id);
		wnd_viewport(wnd);
	}
	else if (strcmp(arg, "map") == 0){

	}
/* window goes from visibile to invisible state */
	else if (strcmp(arg, "unmap") == 0){

	}
/* window changes position or hierarchy, the size part is tied to the
 * buffer in shmif- parlerance so we don't really care to match that
 * here */
	else if (strcmp(arg, "configure") == 0){
		if (!arg_lookup(cmd, "id", 0, &arg)){
			trace(TRACE_XWL, "malformed surface argument: missing surface id");
			goto cleanup;
		}
		uint32_t id = strtoul(arg, NULL, 10);
		struct xwl_window* wnd = xwl_find(id);
		if (!wnd){
			trace(TRACE_XWL, "configure on unknown id %"PRIu32, id);
			goto cleanup;
		}

/* we cache the viewport event for the window as well as for the surface
 * due to the possibility of the unpaired state */
		if (arg_lookup(cmd, "x", 0, &arg)){
			wnd->viewport.ext.viewport.x = strtol(arg, NULL, 10);
		}
		if (arg_lookup(cmd, "y", 0, &arg)){
			wnd->viewport.ext.viewport.y = strtol(arg, NULL, 10);
		}

/* and either reflect now or later */
		wnd_viewport(wnd);
	}

cleanup:
	arg_cleanup(cmd);
}

/*
 * Process / update the incoming pipe, or spawn/respawn the WM if it doesn't
 * exist. This will synch the map- table of known surface IDs that we want to
 * pair with surfaces.
 */
static void xwl_check_wm()
{
	if (xwl_wm_pid == -1){
		trace(TRACE_XWL, "spawning 'arcan-xwayland-wm'");
		int p2c_pipe[2];
		int c2p_pipe[2];
		if (-1 == pipe(p2c_pipe))
			return;

		if (-1 == pipe(c2p_pipe)){
			close(p2c_pipe[0]);
			close(p2c_pipe[1]);
			return;
		}

		wmfd_input = c2p_pipe[0];
		wmfd_output = fdopen(p2c_pipe[1], "w");

		xwl_wm_pid = fork();

		if (-1 == xwl_wm_pid){
			fprintf(stderr, "Couldn't spawn wm- process (fork failed)\n");
			exit(EXIT_FAILURE);
		}

/* child, close, dup spawn */
		if (!xwl_wm_pid){
			char* const argv[] = {"arcan-xwayland-wm", NULL};
			close(p2c_pipe[1]);
			close(c2p_pipe[0]);
			dup2(p2c_pipe[0], STDIN_FILENO);
			dup2(c2p_pipe[1], STDOUT_FILENO);
			close(p2c_pipe[1]);
			close(c2p_pipe[0]);
			execvp("arcan-xwayland-wm", argv);
			execv("arcan-xwayland-wm", argv);
			exit(EXIT_FAILURE);
		}

/* want the input-pipe to work non-blocking here */
		int flags = fcntl(wmfd_input, F_GETFL);
			if (-1 != flags)
				fcntl(wmfd_input, F_SETFL, flags | O_NONBLOCK);

/* drop child write end, parent read end as the wm process owns these now */
		close(c2p_pipe[1]);
		close(p2c_pipe[0]);
	}

/* populate inbuffer, look for linefeed */
	char inbuf[256];
	ssize_t nr = read(wmfd_input, inbuf, 256);
	if (-1 == nr){
		if (errno != EAGAIN && errno != EINTR){
			fclose(wmfd_output);
			close(wmfd_input);
			wmfd_ofs = 0;
			kill(xwl_wm_pid, SIGKILL);
			waitpid(xwl_wm_pid, NULL, 0);
			xwl_wm_pid = -1;
			wmfd_input = -1;
			trace(TRACE_XWL, "arcan-xwayland-wm died");
		}
		return;
	}

/* check the new input for a linefeed character, or flush to the buffer */
	for (size_t i = 0; i < nr; i++){
		if (inbuf[i] == '\n'){
			wmfd_inbuf[wmfd_ofs] = '\0';
			process_input(wmfd_inbuf);
			wmfd_ofs = 0;
		}
/* accept crop on overflow (though no command should be this long) */
		else {
			wmfd_inbuf[wmfd_ofs] = inbuf[i];
			wmfd_ofs = (wmfd_ofs + 1) % sizeof(wmfd_inbuf);
		}
	}
}

static bool xwlsurf_shmifev_handler(
	struct comp_surf* surf, struct arcan_event* ev)
{
	if (ev->category != EVENT_TARGET || !wmfd_output)
		return false;

/* translate relevant non-input shmif events to the text- based
 * format used with the wm- process */

	struct xwl_window* wnd =
		xwl_find_surface(wl_resource_get_id(surf->shell_res));
	if (!wnd)
		return false;

	switch (ev->tgt.kind){
	case TARGET_COMMAND_DISPLAYHINT:{
		struct surf_state states = surf->states;
		bool changed = displayhint_handler(surf, &ev->tgt);
		int rw = ev->tgt.ioevs[0].iv;
		int rh = ev->tgt.ioevs[1].iv;
		int dw = rw - (int)surf->acon.w;
		int dh = rh - (int)surf->acon.h;

/* split up into resize requests and focus/input changes */
		if (rw > 0 && rh > 0 && (dw != 0 || dh != 0)){
			trace(TRACE_XWL, "displayhint: %"PRIu32",%"PRIu32,
				ev->tgt.ioevs[0].iv, ev->tgt.ioevs[1].iv);

			fprintf(wmfd_output,
				"id=%"PRIu32":kind=resize:width=%"PRIu32":height=%"PRIu32"%s\n",
				(uint32_t) wnd->id,
				(uint32_t) abs(ev->tgt.ioevs[0].iv),
				(uint32_t) abs(ev->tgt.ioevs[1].iv),
				(surf->states.drag_resize) ? ":drag" : ""
			);
		}

		if (changed){
			if (states.unfocused != surf->states.unfocused){
				fprintf(wmfd_output, "id=%"PRIu32"%s\n",
					surf->id, states.unfocused ? ":kind=unfocus" : ":kind=focus");
			}
		}

		fflush(wmfd_output);
		return true;
	}
	case TARGET_COMMAND_EXIT:
		fprintf(wmfd_output, "kind=destroy:id=%"PRIu32"\n", wnd->id);
		fflush(wmfd_output);
		return true;
	default:
	break;
	}

	return false;
}

static bool xwl_defer_handler(
	struct surface_request* req, struct arcan_shmif_cont* con)
{
	if (!req || !con){
		return false;
	}

	struct comp_surf* surf = wl_resource_get_user_data(req->target);
	surf->acon = *con;
	surf->cookie = 0xfeedface;
	surf->shell_res = req->target;
	surf->dispatch = xwlsurf_shmifev_handler;
	surf->id = wl_resource_get_id(surf->shell_res);

	struct xwl_window* wnd = (req->tag);
	wnd->surf = surf;
	wnd_viewport(wnd);

	return true;
}

static struct xwl_window*
	lookup_surface(struct comp_surf* surf, struct wl_resource* res)
{
	if (!wl.use_xwayland)
		return NULL;

/* always start by synching against pending from wm as the surface + atom
 * mapping might be done there before we actually get to this stage */
	xwl_check_wm();
	uint32_t id = wl_resource_get_id(res);
	struct xwl_window* wnd = xwl_find_surface(id);
	if (!wnd){
		wnd = xwl_find(0);
		if (!wnd){
			trace(TRACE_XWL, "out-of-memory");
			return NULL;
		}
		wnd->surface_id = id;
		wnd->segid = SEGID_APPLICATION;
	}
	else if (!wnd->paired){
		trace(TRACE_XWL, "paired %"PRIu32, id);
		wnd->paired = true;
		wnd->surf = surf;
	}
	return wnd;
}

static bool xwl_pair_surface(struct comp_surf* surf, struct wl_resource* res)
{
/* do we know of a matching xwayland- provided surface? */
	struct xwl_window* wnd = lookup_surface(surf, res);
	if (!wnd || !wnd->paired)
		return false;

/* if so, allocate the corresponding arcan- side resource */
	return request_surface(surf->client, &(struct surface_request){
/* SEGID should be X11, but need to patch durden as well */
			.segid = wnd->segid,
			.target = res,
			.trace = "xwl",
			.dispatch = xwl_defer_handler,
			.client = surf->client,
			.source = surf,
			.tag = wnd
	}, 'X');
}
