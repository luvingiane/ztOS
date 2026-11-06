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
#include <time.h>
#include <unistd.h>

static pa_threaded_mainloop *ac_loop;
static pa_context *ac_ctx;

/* ---- the outputs: every sink PipeWire has (the speaker, the AirPlay receivers it finds through
 * avahi), kept up to date by the server's own events, so drawing a list never waits on a socket.
 * What plays is one sink, or several AirPlay receivers joined in a group sink of ours. ---- */
#define AC_MAX 8
#define AC_GROUP "ztos_group"	/* name prefix of the sink that plays on several receivers at once */

struct ac_sink {
	char name[160], desc[96];
	int vol;			/* percent */
	bool airplay, sel;		/* an AirPlay receiver; part of what plays now */
};
static struct ac_sink ac_list[AC_MAX], ac_stage[AC_MAX];
static int ac_n, ac_stage_n;
static char ac_default[160];
static char ac_sel[AC_MAX][160];
static int ac_nsel;
static uint32_t ac_group_mod = PA_INVALID_INDEX;
static char ac_group_name[32];
static int ac_group_gen;
static bool ac_dirty;

static bool ac_is_airplay(const char *name) { return !strncmp(name, "raop_sink.", 10); }

static void ac_sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata) {
	(void)c; (void)userdata;
	if (eol) {
		/* the whole list is in: it replaces the last one */
		memcpy(ac_list, ac_stage, sizeof(ac_list));
		ac_n = ac_stage_n;
		ac_stage_n = 0;
		/* what was chosen and has gone is not chosen any more */
		for (int s = 0; s < ac_nsel;) {
			bool here = false;
			for (int k = 0; k < ac_n; k++)
				if (!strcmp(ac_list[k].name, ac_sel[s])) here = true;
			if (here) { s++; continue; }
			memmove(ac_sel[s], ac_sel[s + 1], sizeof(ac_sel[0]) * (ac_nsel - s - 1));
			ac_nsel--;
		}
		if (!ac_nsel && ac_default[0] && strncmp(ac_default, AC_GROUP, strlen(AC_GROUP))) {
			snprintf(ac_sel[0], sizeof(ac_sel[0]), "%s", ac_default);
			ac_nsel = 1;
		}
		ac_dirty = true;
		return;
	}
	if (!i || ac_stage_n >= AC_MAX) return;
	/* the raw loudspeaker sits behind the boosted "Loudspeaker" (op2_speaker_boost): one is offered */
	if (strstr(i->name, "Speaker__sink") || !strncmp(i->name, AC_GROUP, strlen(AC_GROUP))) return;
	struct ac_sink *s = &ac_stage[ac_stage_n++];
	snprintf(s->name, sizeof(s->name), "%s", i->name);
	snprintf(s->desc, sizeof(s->desc), "%s", i->description ? i->description : i->name);
	s->vol = (int)(((uint64_t)pa_cvolume_avg(&i->volume) * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);	/* rounded: truncating lost a point at every step */
	s->airplay = ac_is_airplay(i->name);
	s->sel = false;
}

static void ac_server_cb(pa_context *c, const pa_server_info *si, void *userdata) {
	(void)userdata;
	if (si && si->default_sink_name) snprintf(ac_default, sizeof(ac_default), "%s", si->default_sink_name);
	pa_operation *o = pa_context_get_sink_info_list(c, ac_sink_cb, NULL);
	if (o) pa_operation_unref(o);
}

/* ask for the list again (the loop is locked) */
static void ac_refresh_in(pa_context *c) {
	pa_operation *o = pa_context_get_server_info(c, ac_server_cb, NULL);
	if (o) pa_operation_unref(o);
}

/* An output was picked here: from then on a new stream of an application follows it. PipeWire
 * remembers the last output of each application and would send it back there. */
static bool ac_steer;

static void ac_input_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata) {
	(void)userdata;
	if (eol || !i || !ac_steer || !pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)) return;
	pa_operation *o = pa_context_move_sink_input_by_name(c, i->index, ac_nsel > 1 ? ac_group_name : ac_sel[0], NULL, NULL);
	if (o) pa_operation_unref(o);
}

static void ac_event(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
	(void)userdata;
	if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
		if (ac_steer && (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
			pa_operation *o = pa_context_get_sink_input_info(c, idx, ac_input_cb, NULL);
			if (o) pa_operation_unref(o);
		}
		return;
	}
	ac_refresh_in(c);
}

static void ac_state(pa_context *c, void *userdata) {
	(void)userdata;
	if (pa_context_get_state(c) == PA_CONTEXT_READY) {
		pa_context_set_subscribe_callback(c, ac_event, NULL);
		pa_operation *o = pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER |
			PA_SUBSCRIPTION_MASK_SINK_INPUT, NULL, NULL);
		if (o) pa_operation_unref(o);
		ac_refresh_in(c);
	}
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
		if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) {
			/* The server went away (PipeWire restarted): a new connection, tried at most every 2 s.
			 * What was loaded in the old server (the group) is gone with it. */
			static time_t last_try;
			time_t now = time(NULL);
			if (now - last_try < 2) return false;
			last_try = now;
			pa_threaded_mainloop_lock(ac_loop);
			pa_context_unref(ac_ctx);
			ac_ctx = pa_context_new(pa_threaded_mainloop_get_api(ac_loop), "ztOS shell");
			if (ac_ctx) {
				pa_context_set_state_callback(ac_ctx, ac_state, NULL);
				pa_context_connect(ac_ctx, NULL, PA_CONTEXT_NOFAIL, NULL);
			}
			ac_group_mod = PA_INVALID_INDEX;
			ac_nsel = 0;		/* the list says again what the server plays on */
			ac_steer = false;
			pa_threaded_mainloop_unlock(ac_loop);
			if (!ac_ctx) return false;
			continue;
		}
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

/* Several outputs play together: the master volume is the loudest of them (-1 if only one plays),
 * so the keys never start from a level the outputs are not at, as on iOS. */
static int ac_group_master(void) {
	if (!ac_ready()) return -1;
	int m = -1;
	pa_threaded_mainloop_lock(ac_loop);
	if (ac_steer && ac_nsel > 1) {
		for (int s = 0; s < ac_nsel; s++)
			for (int k = 0; k < ac_n; k++)
				if (!strcmp(ac_list[k].name, ac_sel[s]) && ac_list[k].vol > m) m = ac_list[k].vol;
	}
	pa_threaded_mainloop_unlock(ac_loop);
	return m;
}

/* Every playing output moves by the same step, in percent: a receiver set lower stays lower, and
 * none goes past 0 or 100. */
static void ac_group_step(int step) {
	if (!step || !ac_ready()) return;
	pa_threaded_mainloop_lock(ac_loop);
	for (int s = 0; s < ac_nsel; s++) {
		for (int k = 0; k < ac_n; k++) {
			if (strcmp(ac_list[k].name, ac_sel[s])) continue;
			int v = ac_list[k].vol + step;
			v = v < 0 ? 0 : v > 100 ? 100 : v;
			pa_cvolume cv;
			pa_cvolume_set(&cv, 2, (pa_volume_t)((uint64_t)v * PA_VOLUME_NORM / 100));
			pa_operation *o = pa_context_set_sink_volume_by_name(ac_ctx, ac_sel[s], &cv, NULL, NULL);
			if (o) pa_operation_unref(o);
			ac_list[k].vol = v;
		}
	}
	ac_dirty = true;
	pa_threaded_mainloop_unlock(ac_loop);
}

static void ac_move_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata) {
	char *name = userdata;
	if (eol > 0) { free(name); return; }
	/* only applications' streams: PipeWire's own (a group's legs, the speaker chain's output) stay put */
	if (!i || !pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME)) return;
	pa_operation *o = pa_context_move_sink_input_by_name(c, i->index, name, NULL, NULL);
	if (o) pa_operation_unref(o);
}

/* the outputs as last heard from the server (the list is asked for again if the connection is new) */
static int ac_outputs(struct ac_sink *out) {
	if (!ac_ready()) return 0;
	pa_threaded_mainloop_lock(ac_loop);
	int n = ac_n;
	for (int k = 0; k < n; k++) {
		out[k] = ac_list[k];
		out[k].sel = false;
		for (int s = 0; s < ac_nsel; s++)
			if (!strcmp(ac_sel[s], ac_list[k].name)) out[k].sel = true;
	}
	ac_dirty = false;
	pa_threaded_mainloop_unlock(ac_loop);
	return n;
}

/* the list changed since ac_outputs() was last called */
static bool ac_changed(void) { return ac_dirty; }

static void ac_refresh(void) {
	if (!ac_ready()) return;
	pa_threaded_mainloop_lock(ac_loop);
	ac_refresh_in(ac_ctx);
	pa_threaded_mainloop_unlock(ac_loop);
}

/* The group sink appears a moment after its module loads: ask again every 100 ms (3 s at most),
 * then make it the default output and move what is playing onto it. */
static int ac_group_tries;
static bool ac_group_seen;
static void ac_group_poll(pa_mainloop_api *api, pa_time_event *e, const struct timeval *tv, void *userdata);

static void ac_group_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata) {
	(void)userdata;
	if (i) {
		ac_group_seen = true;
		pa_operation *o = pa_context_set_default_sink(c, ac_group_name, NULL, NULL);
		if (o) pa_operation_unref(o);
		o = pa_context_get_sink_input_info_list(c, ac_move_cb, strdup(ac_group_name));
		if (o) pa_operation_unref(o);
		return;
	}
	if (eol && !ac_group_seen && ac_group_tries < 30) {
		ac_group_tries++;
		struct timeval tv;
		pa_gettimeofday(&tv);
		pa_timeval_add(&tv, 100 * PA_USEC_PER_MSEC);
		pa_mainloop_api *api = pa_threaded_mainloop_get_api(ac_loop);
		api->time_new(api, &tv, ac_group_poll, NULL);
	}
}

static void ac_group_poll(pa_mainloop_api *api, pa_time_event *e, const struct timeval *tv, void *userdata) {
	(void)tv; (void)userdata;
	api->time_free(e);
	pa_operation *o = pa_context_get_sink_info_by_name(ac_ctx, ac_group_name, ac_group_cb, NULL);
	if (o) pa_operation_unref(o);
}

static void ac_group_loaded(pa_context *c, uint32_t idx, void *userdata) {
	(void)userdata;
	if (idx == PA_INVALID_INDEX) return;
	ac_group_mod = idx;
	ac_group_seen = false;
	ac_group_tries = 0;
	pa_operation *o = pa_context_get_sink_info_by_name(c, ac_group_name, ac_group_cb, NULL);
	if (o) pa_operation_unref(o);
}

/* A receiver's connection can be dead while PipeWire still counts it as running (the receiver let
 * go of the session): sound goes nowhere until it is torn down. Suspending the sink and waking it
 * 0.7 s later makes the next sound open a new session, and every receiver chosen together starts
 * at once. */
static void ac_kick_resume(pa_mainloop_api *api, pa_time_event *e, const struct timeval *tv, void *userdata) {
	(void)tv;
	char *name = userdata;
	api->time_free(e);
	pa_operation *o = pa_context_suspend_sink_by_name(ac_ctx, name, 0, NULL, NULL);
	if (o) pa_operation_unref(o);
	free(name);
}

static void ac_kick(const char *name) {	/* the loop is locked */
	pa_operation *o = pa_context_suspend_sink_by_name(ac_ctx, name, 1, NULL, NULL);
	if (o) pa_operation_unref(o);
	struct timeval tv;
	pa_gettimeofday(&tv);
	pa_timeval_add(&tv, 700 * PA_USEC_PER_MSEC);
	pa_mainloop_api *api = pa_threaded_mainloop_get_api(ac_loop);
	api->time_new(api, &tv, ac_kick_resume, strdup(name));
}

/* Play on what is chosen: one sink is made the default itself, several are joined into a group
 * sink (a new name each time, so the old one going away cannot clash with it). */
static void ac_apply(void) {
	if (!ac_ready()) return;
	pa_threaded_mainloop_lock(ac_loop);
	ac_steer = true;
	for (int s = 0; s < ac_nsel; s++)
		if (ac_is_airplay(ac_sel[s])) ac_kick(ac_sel[s]);
	uint32_t old = ac_group_mod;
	ac_group_mod = PA_INVALID_INDEX;
	pa_operation *o;
	if (ac_nsel == 1) {
		o = pa_context_set_default_sink(ac_ctx, ac_sel[0], NULL, NULL);
		if (o) pa_operation_unref(o);
		o = pa_context_get_sink_input_info_list(ac_ctx, ac_move_cb, strdup(ac_sel[0]));
		if (o) pa_operation_unref(o);
		if (old != PA_INVALID_INDEX) {
			o = pa_context_unload_module(ac_ctx, old, NULL, NULL);
			if (o) pa_operation_unref(o);
		}
	} else if (ac_nsel > 1) {
		if (old != PA_INVALID_INDEX) {
			o = pa_context_unload_module(ac_ctx, old, NULL, NULL);
			if (o) pa_operation_unref(o);
		}
		snprintf(ac_group_name, sizeof(ac_group_name), AC_GROUP "%d", ++ac_group_gen);
		char args[AC_MAX * 170 + 128];
		int len = snprintf(args, sizeof(args), "sink_name=%s sink_properties=device.description=ztOS-AirPlay slaves=", ac_group_name);
		for (int s = 0; s < ac_nsel && len < (int)sizeof(args) - 170; s++)
			len += snprintf(args + len, sizeof(args) - len, "%s%s", s ? "," : "", ac_sel[s]);
		o = pa_context_load_module(ac_ctx, "module-combine-sink", args, ac_group_loaded, NULL);
		if (o) pa_operation_unref(o);
	}
	pa_threaded_mainloop_unlock(ac_loop);
}

/* a name can go in a module's argument list only if it holds no separator or quote */
static bool ac_name_ok(const char *n) {
	for (; *n; n++)
		if (*n == ' ' || *n == ',' || *n == '"' || *n == '\'' || *n == '=') return false;
	return true;
}

/* A tap on a row: the output joins what plays, or leaves it (one always stays). The speaker and the
 * AirPlay receivers do not mix: a receiver plays about two seconds late, the speaker at once. */
static void ac_toggle(const char *name) {
	if (!name || !*name || !ac_ready()) return;
	pa_threaded_mainloop_lock(ac_loop);
	int at = -1;
	for (int s = 0; s < ac_nsel; s++)
		if (!strcmp(ac_sel[s], name)) at = s;
	bool known = false;
	for (int k = 0; k < ac_n; k++)
		if (!strcmp(ac_list[k].name, name)) known = true;
	if (!known) {
		pa_threaded_mainloop_unlock(ac_loop);
		return;
	}
	if (at >= 0) {
		if (ac_nsel > 1) {
			memmove(ac_sel[at], ac_sel[at + 1], sizeof(ac_sel[0]) * (ac_nsel - at - 1));
			ac_nsel--;
		}
	} else {
		if (!ac_is_airplay(name) || !ac_name_ok(name)) ac_nsel = 0;	/* alone */
		for (int s = 0; s < ac_nsel;) {
			if (ac_is_airplay(ac_sel[s]) && ac_name_ok(ac_sel[s])) { s++; continue; }
			memmove(ac_sel[s], ac_sel[s + 1], sizeof(ac_sel[0]) * (ac_nsel - s - 1));
			ac_nsel--;
		}
		if (ac_nsel < AC_MAX) snprintf(ac_sel[ac_nsel++], sizeof(ac_sel[0]), "%s", name);
	}
	ac_dirty = true;
	pa_threaded_mainloop_unlock(ac_loop);
	ac_apply();
}

/* one output's own volume, in percent (the keys' volume is the default sink's: the group's, if several play) */
static void ac_set_sink_volume(const char *name, int percent) {
	if (!name || !*name || !ac_ready()) return;
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	pa_cvolume cv;
	pa_cvolume_set(&cv, 2, (pa_volume_t)((uint64_t)percent * PA_VOLUME_NORM / 100));
	pa_threaded_mainloop_lock(ac_loop);
	pa_operation *o = pa_context_set_sink_volume_by_name(ac_ctx, name, &cv, NULL, NULL);
	if (o) pa_operation_unref(o);
	for (int k = 0; k < ac_n; k++)
		if (!strcmp(ac_list[k].name, name)) ac_list[k].vol = percent;
	ac_dirty = true;
	pa_threaded_mainloop_unlock(ac_loop);
}
#endif
