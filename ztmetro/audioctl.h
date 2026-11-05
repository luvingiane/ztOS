/*
 * audioctl.h: the volume and the output, asked of PipeWire (PulseAudio protocol) from inside the
 * shell, on a connection kept open. The volume keys and the output switch used to start wpctl and
 * a chain of pactl processes (0.3 s each on a Cortex-A53, a second for the switch): the answer was
 * late. Here a call is a message on an open socket. Every call is fire-and-forget; if the server is
 * not there yet it is dropped and tried again at the next call.
 */
#ifndef ZTOS_AUDIOCTL_H
#define ZTOS_AUDIOCTL_H
#include <pulse/pulseaudio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pa_threaded_mainloop *ac_loop;
static pa_context *ac_ctx;

static void ac_state(pa_context *c, void *userdata) {
	(void)c; (void)userdata;
	pa_threaded_mainloop_signal(ac_loop, 0);
}

/* connected (or on the way: waits up to 300 ms the first time) */
static bool ac_ready(void) {
	if (!ac_loop) {
		ac_loop = pa_threaded_mainloop_new();
		if (!ac_loop) return false;
		ac_ctx = pa_context_new(pa_threaded_mainloop_get_api(ac_loop), "ztOS shell");
		if (!ac_ctx) return false;
		pa_context_set_state_callback(ac_ctx, ac_state, NULL);
		pa_threaded_mainloop_start(ac_loop);
		pa_threaded_mainloop_lock(ac_loop);
		pa_context_connect(ac_ctx, NULL, PA_CONTEXT_NOFAIL, NULL);
		pa_threaded_mainloop_unlock(ac_loop);
	}
	for (int i = 0; i < 30; i++) {
		pa_threaded_mainloop_lock(ac_loop);
		pa_context_state_t st = pa_context_get_state(ac_ctx);
		pa_threaded_mainloop_unlock(ac_loop);
		if (st == PA_CONTEXT_READY) return true;
		if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) return false;
		usleep(10000);
	}
	return false;
}

/* the default output's volume, in percent */
static void ac_set_volume(int percent) {
	if (!ac_ready()) return;
	if (percent < 0) percent = 0;
	if (percent > 150) percent = 150;
	pa_cvolume cv;
	pa_cvolume_set(&cv, 2, (pa_volume_t)((uint64_t)percent * PA_VOLUME_NORM / 100));
	pa_threaded_mainloop_lock(ac_loop);
	pa_operation *o = pa_context_set_sink_volume_by_name(ac_ctx, "@DEFAULT_SINK@", &cv, NULL, NULL);
	if (o) pa_operation_unref(o);
	pa_threaded_mainloop_unlock(ac_loop);
}

static void ac_move_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata) {
	char *name = userdata;
	if (eol > 0) { free(name); return; }
	if (!i) return;
	pa_operation *o = pa_context_move_sink_input_by_name(c, i->index, name, NULL, NULL);
	if (o) pa_operation_unref(o);
}

/* make it the default output and move what is playing onto it (the name is copied) */
static void ac_set_output(const char *name) {
	if (!name || !*name || !ac_ready()) return;
	pa_threaded_mainloop_lock(ac_loop);
	pa_operation *o = pa_context_set_default_sink(ac_ctx, name, NULL, NULL);
	if (o) pa_operation_unref(o);
	o = pa_context_get_sink_input_info_list(ac_ctx, ac_move_cb, strdup(name));
	if (o) pa_operation_unref(o);
	pa_threaded_mainloop_unlock(ac_loop);
}
#endif
