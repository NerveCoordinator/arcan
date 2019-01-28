#include <arcan_shmif.h>
#include <arcan_shmif_server.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

int main(int argc, char** argv)
{
/* setup our clock */
	shmifsrv_monotonic_rebase();

/* setup listening point */
	struct shmifsrv_client* cl =
		shmifsrv_allocate_connpoint("shmifsrv", NULL, S_IRWXU, -1);

	if (!cl){
		fprintf(stderr, "couldn't allocate connection point\n");
		return EXIT_FAILURE;
	}

	int server_fd = shmifsrv_client_handle(cl);

/* wait for a connection */
	while (true){
		struct pollfd pfd = {
			.fd = server_fd,
			.events = POLLIN | POLLERR | POLLHUP
		};

		int rv = poll(&pfd, 1, -1);
		if (-1 == rv){
			if (errno == EAGAIN || errno == EINTR)
				continue;
			break;
		}

/* process client until its death */
		int sv;
		while ((sv = shmifsrv_poll(cl)) != CLIENT_DEAD){
			if (sv == CLIENT_VBUFFER_READY){
				struct shmifsrv_vbuffer buf = shmifsrv_video(cl, true);
				fprintf(stderr, "[video] : %zu*%zu\n", buf.w, buf.h);
			}
			else if (sv == CLIENT_ABUFFER_READY){
				struct shmifsrv_abuffer buf = shmifsrv_audio(cl, NULL, 0);
				fprintf(stderr,
					"[audio], %zu samples @ %zu Hz", buf.samples, buf.samplerate);
			}

	/* flush out events */
		struct arcan_event ev;
		while (1 == shmifsrv_dequeue_events(cl, &ev, 1)){
/* PREROLL stage, need to send ACTIVATE */
			if (ev.ext.kind == EVENT_EXTERNAL_REGISTER){
				shmifsrv_enqueue_event(cl, &(struct arcan_event){
					.category = EVENT_TARGET,
					.tgt.kind = TARGET_COMMAND_ACTIVATE
				}, -1);
			}
/* always reject requests for additional segments */
			else if (ev.ext.kind == EVENT_EXTERNAL_SEGREQ){
				shmifsrv_enqueue_event(cl, &(struct arcan_event){
					.category = EVENT_TARGET,
					.tgt.kind = TARGET_COMMAND_REQFAIL,
					.tgt.ioevs[0].iv = ev.ext.segreq.id
				}, -1);
			}
			else if (shmifsrv_process_event(cl, &ev))
				continue;
		}

/* let the monotonic clock drive timers etc. */
		int ticks = shmifsrv_monotonic_tick(NULL);
		while(ticks--)
			shmifsrv_tick(cl);
	}

	shmifsrv_free(cl);
	return EXIT_SUCCESS;
}
