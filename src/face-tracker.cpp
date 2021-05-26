#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include "plugin-macros.generated.h"
#include "face-detector-base.h"
#include "face-detector-dlib.h"
#include "face-tracker-base.h"
#include "face-tracker-dlib.h"
#include <algorithm>
#include <deque>
#include <math.h>
#include <graphics/matrix4.h>

struct f4
{
	float v[4];

	f4 (const f4 &a) {*this=a;}
	f4 (float a, float b, float c, float d) { v[0]=a; v[1]=b; v[2]=c; v[3]=d; }
	f4 (const rect_s &a) { v[0]=(float)a.x0; v[1]=(float)a.y0; v[2]=(float)a.x1; v[3]=(float)a.y1; }
	f4 operator + (const f4 &a) { return f4 (v[0]+a.v[0], v[1]+a.v[1], v[2]+a.v[2], v[3]+a.v[3]); }
	f4 operator * (float a) { return f4 (v[0]*a, v[1]*a, v[2]*a, v[3]*a); }
	f4 & operator += (const f4 &a) { return *this = f4 (v[0]+a.v[0], v[1]+a.v[1], v[2]+a.v[2], v[3]+a.v[3]); }
};

static inline int get_width (const rect_s &r) { return r.x1 - r.x0; }
static inline int get_height(const rect_s &r) { return r.y1 - r.y0; }

static inline float common_length(float a0, float a1, float b0, float b1)
{
	// assumes a0 < a1, b0 < b1
	// if (a1 <= b0) return 0.0f; // a0 < a1 < b0 < b1
	if (a0 <= b0 && b0 <= a1 && a1 <= b1) return a1 - b0; // a0 < b0 < a1 < b1
	if (a0 <= b0 && b1 <= a1) return b1 - b0; // a0 < b0 < b1 < a1
	if (b0 <= a0 && a1 <= b1) return a1 - a0; // b0 < a0 < a1 < b1
	if (b0 <= a0 && a0 <= b1 && a0 <= b1) return b1 - a0; // b0 < a0 < b1 < a1
	// if (b1 <= a0) return 0.0f; // b0 < b1 < a0 < a1
	return 0.0f;
}

static inline float common_area(const rect_s &a, const rect_s &b)
{
	return common_length(a.x0, a.x1, b.x0, b.x1) * common_length(a.y0, a.y1, b.y0, b.y1);
}

struct tracker_inst_s
{
	face_tracker_base *tracker;
	rect_s rect;
	rect_s crop_tracker; // crop corresponding to current processing image
	rect_s crop_rect; // crop corresponding to rect
	float att;
	enum tracker_state_e {
		tracker_state_init = 0,
		tracker_state_reset_texture, // texture has been set, position is not set.
		tracker_state_constructing, // texture and positions have been set, starting to construct correlation_tracker.
		tracker_state_first_track, // correlation_tracker has been prepared, running 1st tracking
		tracker_state_available, // 1st tracking was done, `rect` is available, can accept next frame.
		tracker_state_ending,
	} state;
	int tick_cnt;
};

struct face_tracker_filter
{
	obs_source_t *context;
	gs_texrender_t *texrender;
	gs_stagesurf_t* stagesurface;
	uint32_t known_width;
	uint32_t known_height;
	int tick_cnt;
	int next_tick_stage_to_detector;
	bool target_valid;
	bool rendered;
	bool staged;
	bool is_active;
	bool detector_in_progress;

	face_detector_base *detect;
	std::vector<rect_s> *rects;
	int detect_tick;

	std::deque<struct tracker_inst_s> *trackers;
	std::deque<struct tracker_inst_s> *trackers_idlepool;

	rect_s crop_cur;
	rect_s detect_crop;
	rect_s detect_err;

	float upsize_l, upsize_r, upsize_t, upsize_b;
	float track_z, track_x, track_y;
	float scale_max;

	float kp;
	float klpf;
	float tlpf;
	f4 filter_int_out;
	f4 filter_lpf;

	bool debug_faces;
	bool debug_notrack;
};

static const char *ftf_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Face Tracker");
}

static void ftf_update(void *data, obs_data_t *settings)
{
	auto *s = (struct face_tracker_filter*)data;

	s->upsize_l = obs_data_get_double(settings, "upsize_l");
	s->upsize_r = obs_data_get_double(settings, "upsize_r");
	s->upsize_t = obs_data_get_double(settings, "upsize_t");
	s->upsize_b = obs_data_get_double(settings, "upsize_b");
	s->track_z = obs_data_get_double(settings, "track_z");
	s->track_x = obs_data_get_double(settings, "track_x");
	s->track_y = obs_data_get_double(settings, "track_y");
	s->scale_max = obs_data_get_double(settings, "scale_max");

	double kp = obs_data_get_double(settings, "Kp");
	double td = obs_data_get_double(settings, "Td");
	s->kp = (float)kp;
	s->klpf = (float)(td * kp);
	s->tlpf = (float)obs_data_get_double(settings, "Tdlpf");

	s->debug_faces = obs_data_get_bool(settings, "debug_faces");
	s->debug_notrack = obs_data_get_bool(settings, "debug_notrack");
}

static void *ftf_create(obs_data_t *settings, obs_source_t *context)
{
	auto *s = (struct face_tracker_filter*)bzalloc(sizeof(struct face_tracker_filter));
	s->rects = new std::vector<rect_s>;
	s->crop_cur.x1 = s->crop_cur.y1 = -1;
	s->context = context;
	s->detect = new face_detector_dlib();
	s->detect->start();
	s->trackers = new std::deque<struct tracker_inst_s>;
	s->trackers_idlepool = new std::deque<struct tracker_inst_s>;

	obs_source_update(context, settings);
	return s;
}

static void ftf_destroy(void *data)
{
	auto *s = (struct face_tracker_filter*)data;

	obs_enter_graphics();
	gs_texrender_destroy(s->texrender);
	s->texrender = NULL;
	gs_stagesurface_destroy(s->stagesurface);
	s->stagesurface = NULL;
	obs_leave_graphics();

	s->detect->stop();

	delete s->rects;
	delete s->trackers;
	delete s->trackers_idlepool;
	bfree(s);
}

static obs_properties_t *ftf_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props;
	props = obs_properties_create();

	obs_properties_add_float(props, "upsize_l", "Upsize Left", -0.4, 4.0, 0.2);
	obs_properties_add_float(props, "upsize_r", "Upsize Right", -0.4, 4.0, 0.2);
	obs_properties_add_float(props, "upsize_t", "Upsize Top", -0.4, 4.0, 0.2);
	obs_properties_add_float(props, "upsize_b", "Upsize Bottom", -0.4, 4.0, 0.2);
	obs_properties_add_float(props, "track_z", "Tracking Zoom", 0.1, 2.0, 0.1);
	obs_properties_add_float(props, "track_x", "Tracking X", -1.0, +1.0, 0.05);
	obs_properties_add_float(props, "track_y", "Tracking Y", -1.0, +1.0, 0.05);
	obs_properties_add_float(props, "scale_max", "Scale max", 1.0, 20.0, 1.0);

	obs_properties_add_float(props, "Kp", "Track Kp", 0.01, 10.0, 0.1);
	obs_properties_add_float(props, "Td", "Track Td", 0.0, 5.0, 0.01);
	obs_properties_add_float(props, "Tdlpf", "Track LPF for Td", 0.0, 2.0, 0.002);

	obs_properties_add_bool(props, "debug_faces", "Show face detection results");
	obs_properties_add_bool(props, "debug_notrack", "Stop tracking faces");

	return props;
}

static void ftf_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "upsize_l", 0.2);
	obs_data_set_default_double(settings, "upsize_r", 0.2);
	obs_data_set_default_double(settings, "upsize_t", 0.3);
	obs_data_set_default_double(settings, "upsize_b", 0.1);
	obs_data_set_default_double(settings, "track_z",  0.70); //  1.00  0.50  0.35
	obs_data_set_default_double(settings, "track_y", +0.00); // +0.00 +0.10 +0.30
	obs_data_set_default_double(settings, "scale_max", 10.0);

	obs_data_set_default_double(settings, "Kp", 0.5);
	obs_data_set_default_double(settings, "Td", 0.05);
	obs_data_set_default_double(settings, "Tdlpf", 0.025);
}

static void tick_filter(struct face_tracker_filter *s, float second)
{
	f4 e = s->detect_err;

	s->filter_int_out += e * (second * s->kp);
	s->filter_lpf = (s->filter_lpf * s->tlpf + e * second) * (1.f/(s->tlpf + second));

	f4 u = s->filter_int_out + s->filter_lpf * s->klpf;

	const float h = s->known_height / s->scale_max;
	const float w = s->known_width / s->scale_max;
	const float h1 = s->known_height - h;
	const float w1 = s->known_width - w;

	const f4 range_min(0.0f, 0.0f, w, h);
	const f4 range_max(w1, h1, s->known_width, s->known_height);
	for (int i=0; i<4; i++) {
		if (u.v[i] < range_min.v[i]) {
			u.v[0] = range_min.v[i];
			if (s->filter_int_out.v[i] < range_min.v[i])
				s->filter_int_out.v[i] = range_min.v[i];
		}
		else if (u.v[i] > range_max.v[i]) {
			u.v[i] = range_max.v[i];
			if (s->filter_int_out.v[i] > range_max.v[i])
				s->filter_int_out.v[i] = range_max.v[i];
		}
	}

	s->crop_cur.x0 = (int)u.v[0];
	s->crop_cur.y0 = (int)u.v[1];
	s->crop_cur.x1 = (int)u.v[2];
	s->crop_cur.y1 = (int)u.v[3];
}

static void ftf_activate(void *data)
{
	auto *s = (struct face_tracker_filter*)data;
	s->is_active = true;
}

static void ftf_deactivate(void *data)
{
	auto *s = (struct face_tracker_filter*)data;
	s->is_active = false;
}

static inline void calculate_error(struct face_tracker_filter *s);

static void ftf_tick(void *data, float second)
{
	auto *s = (struct face_tracker_filter*)data;
	const bool was_rendered = s->rendered;
	if (s->detect_tick==s->tick_cnt)
		s->next_tick_stage_to_detector = s->tick_cnt + (int)(2.0f/second); // detect for each _ second(s).

	s->tick_cnt += 1;

	obs_source_t *target = obs_filter_get_target(s->context);
	if (!target)
		goto err;

	s->rendered = false;

	s->known_width = obs_source_get_base_width(target);
	s->known_height = obs_source_get_base_height(target);

	if (s->known_width<=0 || s->known_height<=0)
		goto err;

	if (s->crop_cur.x1<0 || s->crop_cur.y1<0) {
		// reset crop_cur
		s->crop_cur.x0 = 0;
		s->crop_cur.y0 = 0;
		s->crop_cur.x1 = s->known_width;
		s->crop_cur.y1 = s->known_height;
		s->filter_int_out = s->crop_cur;
	}
	else if (was_rendered) {
		calculate_error(s);
		tick_filter(s, second);
	}

	s->target_valid = true;
	return;
err:
	s->target_valid = false;
}

static inline void render_target(struct face_tracker_filter *s, obs_source_t *target, obs_source_t *parent)
{
	if (!s->texrender)
		s->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	const uint32_t cx = s->known_width, cy = s->known_height;
	gs_texrender_reset(s->texrender);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(s->texrender, cx, cy)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(s->texrender);
	}

	gs_blend_state_pop();

	s->staged = false;
}

static inline rect_s calculate_error_one(struct face_tracker_filter *s, rect_s d, rect_s crop)
{
	// blog(LOG_INFO, "calculate_error_one: d=%d %d %d %d", d.x0, d.y0, d.x1, d.y1);
	// blog(LOG_INFO, "calculate_error_one: crop=%d %d %d %d", crop.x0, crop.y0, crop.x1, crop.y1);
	const int width = s->known_width;
	const int height = s->known_height;
	if (get_height(d) < 2) {
		rect_s r{0};
		return r;
	}
	float scale = height / get_height(d) * s->track_z;
	if (scale > s->scale_max) scale = s->scale_max;
	else if (scale < 1.0f) scale = 1.0f;
	float cx = (d.x0 + d.x1) * 0.5f;
	float cy = (d.y0 + d.y1) * 0.5f;
	float w = width / scale;
	float h = height / scale;
	d.x0 = cx - w/2 - width * s->track_x / 2;
	d.x1 = cx + w/2 - width * s->track_x / 2;
	d.y0 = cy - h/2 + height * s->track_y / 2;
	d.y1 = cy + h/2 + height * s->track_y / 2;
	if (d.x0 < 0) { d.x1 -= d.x0; d.x0 = 0; }
	if (d.x1 > width) { d.x0 += width-d.x1; d.x1 = width; }
	if (d.y0 < 0) { d.y1 -= d.y0; d.y0 = 0; }
	if (d.y1 > height) { d.y0 += height-d.y1; d.y1 = height; }

	// blog(LOG_INFO, "calculate_error_one: expected: %d %d %d %d", d.x0, d.y0, d.x1, d.y1);
	d.x0 -= crop.x0;
	d.x1 -= crop.x1;
	d.y0 -= crop.y0;
	d.y1 -= crop.y1;
	// blog(LOG_INFO, "calculate_error_one: error: %d %d %d %d score=%f", d.x0, d.y0, d.x1, d.y1, d.score);
	return d;
}

static inline void calculate_error(struct face_tracker_filter *s)
{
	// blog(LOG_INFO, "entering calculate_error");
	const int width = s->known_width;
	const int height = s->known_height;
	rect_s e_tot;
	float x0_tot = 0.0f, x1_tot = 0.0f, y0_tot = 0.0f, y1_tot = 0.0f;
	float sc_tot = 0.0f;
	std::deque<struct tracker_inst_s> &trackers = *s->trackers;
	for (int i=0; i<trackers.size(); i++) if (trackers[i].state == tracker_inst_s::tracker_state_available) {
		rect_s r = trackers[i].rect;
		r.score = r.score * trackers[i].att;
		rect_s e = calculate_error_one(s, r, trackers[i].crop_rect);
		x0_tot += e.x0 * e.score;
		y0_tot += e.y0 * e.score;
		x1_tot += e.x1 * e.score;
		y1_tot += e.y1 * e.score;
		sc_tot += e.score;
	}

	// blog(LOG_INFO, "calculate_error %f %f %f %f %f", x0_tot/sc_tot, x1_tot / sc_tot, y0_tot / sc_tot, y1_tot / sc_tot, sc_tot);

	if (sc_tot > 1e-19f) {
		s->detect_err.x0 = x0_tot / sc_tot;
		s->detect_err.x1 = x1_tot / sc_tot;
		s->detect_err.y0 = y0_tot / sc_tot;
		s->detect_err.y1 = y1_tot / sc_tot;
	}
	else {
		s->detect_err.x0 = 0.0f;
		s->detect_err.x1 = 0.0f;
		s->detect_err.y0 = 0.0f;
		s->detect_err.y1 = 0.0f;
	}
}

static inline void retire_tracker(struct face_tracker_filter *s, int ix)
{
	std::deque<struct tracker_inst_s> &trackers = *s->trackers;
	s->trackers_idlepool->push_back(trackers[ix]);
	trackers[ix].tracker->request_suspend();
	trackers.erase(trackers.begin()+ix);
}

static inline void attenuate_tracker(struct face_tracker_filter *s)
{
	std::deque<struct tracker_inst_s> &trackers = *s->trackers;
	std::vector<rect_s> &rects = *s->rects;

	for (int j=0; j<rects.size(); j++) {
		rect_s r = rects[j];
		float a0 = (r.x1 - r.x0) * (r.y1 - r.y0);
		float a_overlap_sum = 0;
		for (int i=trackers.size()-1; i>=0; i--) {
			if (trackers[i].state != tracker_inst_s::tracker_state_available)
				continue;
			float a = common_area(r, trackers[i].rect);
			a_overlap_sum += a;
			if (a>a0*1e-2f && a_overlap_sum > a0*1.0f)
				retire_tracker(s, i);
		}
	}

	for (int i=0; i<trackers.size(); i++) {
		if (trackers[i].state != tracker_inst_s::tracker_state_available)
			continue;
		struct tracker_inst_s &t = trackers[i];

		float a1 = (t.rect.x1 - t.rect.x0) * (t.rect.y1 - t.rect.y0);
		float amax = a1*0.1f;
		for (int j=0; j<rects.size(); j++) {
			rect_s r = rects[j];
			float a0 = (r.x1 - r.x0) * (r.y1 - r.y0);
			float a = common_area(r, t.rect);
			if (a > amax) amax = a;
		}

		t.att *= powf(amax / a1, 0.1f); // if no faces, remove the tracker
	}

	float score_max = 0.0f;
	for (int i=0; i<trackers.size(); i++) {
		if (trackers[i].state == tracker_inst_s::tracker_state_available) {
			float s = trackers[i].att * trackers[i].rect.score;
			if (s > score_max) score_max = s;
		}
	}

	for (int i=0; i<trackers.size(); ) {
		if (trackers[i].state == tracker_inst_s::tracker_state_available && trackers[i].att * trackers[i].rect.score < 1e-2f * score_max) {
			retire_tracker(s, i);
		}
		else
			i++;
	}
	blog(LOG_INFO, "attenuate_tracker: trackers.size=%d total=%d", trackers.size(), trackers.size()+s->trackers_idlepool->size());
}

static inline void copy_detector_to_tracker(struct face_tracker_filter *s)
{
	std::deque<struct tracker_inst_s> &trackers = *s->trackers;
	int i_tracker;
	for (i_tracker=0; i_tracker < trackers.size(); i_tracker++)
		if (
				trackers[i_tracker].tick_cnt == s->detect_tick &&
				trackers[i_tracker].state==tracker_inst_s::tracker_state_e::tracker_state_reset_texture )
			break;
	if (i_tracker >= trackers.size())
		return;

	if (s->rects->size()<=0) {
		trackers.erase(trackers.begin() + i_tracker);
		return;
	}

	struct tracker_inst_s &t = trackers[i_tracker];

	struct rect_s r = (*s->rects)[0];
	int w = r.x1-r.x0;
	int h = r.y1-r.y0;
	r.x0 -= w * s->upsize_l;
	r.x1 += w * s->upsize_r;
	r.y0 -= h * s->upsize_t;
	r.y1 += h * s->upsize_b;
	t.tracker->set_position(r); // TODO: consider how to track two or more faces.
	t.tracker->start();
	t.state = tracker_inst_s::tracker_state_constructing;
}

static inline int stage_to_surface(struct face_tracker_filter *s)
{
	if (s->staged)
		return 0;

	uint32_t width = s->known_width;
	uint32_t height = s->known_height;
	if (width<=0 || height<=0)
		return 1;

	gs_texture_t *tex = gs_texrender_get_texture(s->texrender);
	if (!tex)
		return 2;

	if (!s->stagesurface ||
			width != gs_stagesurface_get_width(s->stagesurface) ||
			height != gs_stagesurface_get_height(s->stagesurface) ) {
		gs_stagesurface_destroy(s->stagesurface);
		s->stagesurface = gs_stagesurface_create(width, height, GS_RGBA);
	}

	gs_stage_texture(s->stagesurface, tex);

	s->staged = true;

	return 0;
}

static inline void stage_to_detector(struct face_tracker_filter *s)
{
	if (s->detect->trylock())
		return;

	// get previous results
	if (s->detector_in_progress) {
		s->detect->get_faces(*s->rects);
		attenuate_tracker(s);
		copy_detector_to_tracker(s);
		s->detector_in_progress = false;
	}

	if ((s->next_tick_stage_to_detector - s->tick_cnt) > 0) {
		// blog(LOG_INFO, "stage_to_detector: waiting next_tick_stage_to_detector=%d tick_cnt=%d", s->next_tick_stage_to_detector, s->tick_cnt);
		s->detect->unlock();
		return;
	}

	if (!stage_to_surface(s)) {
		uint8_t *video_data = NULL;
		uint32_t video_linesize;
		if (gs_stagesurface_map(s->stagesurface, &video_data, &video_linesize)) {
			uint32_t width = s->known_width;
			uint32_t height = s->known_height;
			s->detect->set_texture(video_data, video_linesize, width, height);
			gs_stagesurface_unmap(s->stagesurface);
			s->detect_crop = s->crop_cur;
			s->detect->signal();
			s->detector_in_progress = true;
			s->detect_tick = s->tick_cnt;

			struct tracker_inst_s t;
			if (s->trackers_idlepool->size() > 0) {
				t.tracker = (*s->trackers_idlepool)[0].tracker;
				(*s->trackers_idlepool)[0].tracker = NULL;
				s->trackers_idlepool->pop_front();
			}
			else
				t.tracker = new face_tracker_dlib();
			t.crop_tracker = s->crop_cur;
			t.state = tracker_inst_s::tracker_state_e::tracker_state_reset_texture;
			t.tick_cnt = s->tick_cnt;
			t.tracker->set_texture(video_data, video_linesize, width, height); // TODO: common texture object.
			s->trackers->push_back(t);
		}
	}

	s->detect->unlock();
}

static inline int stage_surface_to_tracker(struct face_tracker_filter *s, struct tracker_inst_s &t)
{
	if (int ret = stage_to_surface(s))
		return ret;

	uint8_t *video_data = NULL;
	uint32_t video_linesize;
	if (gs_stagesurface_map(s->stagesurface, &video_data, &video_linesize)) {
		uint32_t width = s->known_width;
		uint32_t height = s->known_height;
		t.tracker->set_texture(video_data, video_linesize, width, height);
		t.crop_tracker = s->crop_cur;
		gs_stagesurface_unmap(s->stagesurface);
		t.tracker->signal();
	}
	else
		return 1;
	return 0;
}

static inline void stage_to_trackers(struct face_tracker_filter *s)
{
	std::deque<struct tracker_inst_s> &trackers = *s->trackers;
	for (int i=0; i<trackers.size(); i++) {
		struct tracker_inst_s &t = trackers[i];
		if (t.state == tracker_inst_s::tracker_state_constructing) {
			if (!t.tracker->trylock()) {
				if (!stage_surface_to_tracker(s, t)) {
					t.crop_tracker = s->crop_cur;
					t.state = tracker_inst_s::tracker_state_first_track;
				}
				t.tracker->unlock();
				t.state = tracker_inst_s::tracker_state_first_track;
			}
		}
		else if (t.state == tracker_inst_s::tracker_state_first_track) {
			if (!t.tracker->trylock()) {
				t.tracker->get_face(t.rect);
				t.crop_rect = t.crop_tracker;
				// blog(LOG_INFO, "tracker_state_first_track: %p rect=%d %d %d %d %f", t.tracker, t.rect.x0, t.rect.y0, t.rect.x1, t.rect.y1, t.rect.score);
				t.att = 1.0f;
				stage_surface_to_tracker(s, t);
				t.tracker->signal();
				t.tracker->unlock();
				t.state = tracker_inst_s::tracker_state_available;
			}
		}
		else if (t.state == tracker_inst_s::tracker_state_available) {
			if (!t.tracker->trylock()) {
				t.tracker->get_face(t.rect);
				t.crop_rect = t.crop_tracker;
				// blog(LOG_INFO, "tracker_state_available: %p rect=%d %d %d %d %f", t.tracker, t.rect.x0, t.rect.y0, t.rect.x1, t.rect.y1, t.rect.score);
				stage_surface_to_tracker(s, t);
				t.tracker->signal();
				t.tracker->unlock();
			}
		}
	}
}

static inline void draw_frame(struct face_tracker_filter *s)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_texture_t *tex = gs_texrender_get_texture(s->texrender);
	if (!tex)
		return;

	uint32_t width = s->known_width;
	uint32_t height = s->known_height;

	// TODO: linear_srgb, 27 only?

	gs_matrix_push();
	if (width>0 && height>0) {
		const rect_s &crop_cur = s->crop_cur;
		float scale = sqrtf((float)(width*height) / ((crop_cur.x1-crop_cur.x0) * (crop_cur.y1-crop_cur.y0)));
		struct matrix4 tr;
		matrix4_identity(&tr);
		matrix4_translate3f(&tr, &tr, -(crop_cur.x0+crop_cur.x1)*0.5f, -(crop_cur.y0+crop_cur.y1)*0.5f, 0.0f);
		matrix4_scale3f(&tr, &tr, scale, scale, 1.0f);
		matrix4_translate3f(&tr, &tr, width/2, height/2, 0.0f);
		if (!(s->debug_notrack && !s->is_active))
			gs_matrix_mul(&tr);

		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(image, tex);
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(tex, 0, width, height);
		}
	}

	if (s->debug_faces && !s->is_active) {
		effect = obs_get_base_effect(OBS_EFFECT_SOLID);
		while (gs_effect_loop(effect, "Solid")) {
			gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFF0000FF);
			for (int i=0; i<s->rects->size(); i++) {
				rect_s r = (*s->rects)[i];
				if (r.x0>=r.x1 || r.y0>=r.y1)
					continue;
				int w = r.x1-r.x0;
				int h = r.y1-r.y0;
				gs_render_start(true);
				gs_vertex2f(r.x0, r.y0);
				gs_vertex2f(r.x0, r.y1);
				gs_vertex2f(r.x1, r.y1);
				gs_vertex2f(r.x1, r.y0);
				gs_vertex2f(r.x0, r.y0);
				r.x0 -= w * s->upsize_l;
				r.x1 += w * s->upsize_r;
				r.y0 -= h * s->upsize_t;
				r.y1 += h * s->upsize_b;
				//gs_render_start(false);
				gs_vertex2f(r.x0, r.y0);
				gs_vertex2f(r.x0, r.y1);
				gs_vertex2f(r.x1, r.y1);
				gs_vertex2f(r.x1, r.y0);
				gs_vertex2f(r.x0, r.y0);
				gs_vertbuffer_t *vb = gs_render_save();
				gs_load_vertexbuffer(vb);
				gs_draw(GS_LINESTRIP, 0, 0);
				gs_vertexbuffer_destroy(vb);
			}
			gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFF00FF00);
			for (int i=0; i<s->trackers->size(); i++) {
				tracker_inst_s &t = (*s->trackers)[i];
				if (t.state != tracker_inst_s::tracker_state_available)
					continue;
				rect_s r = t.rect;
				if (r.x0>=r.x1 || r.y0>=r.y1)
					continue;
				int w = r.x1-r.x0;
				int h = r.y1-r.y0;
				gs_render_start(true);
				gs_vertex2f(r.x0, r.y0);
				gs_vertex2f(r.x0, r.y1);
				gs_vertex2f(r.x1, r.y1);
				gs_vertex2f(r.x1, r.y0);
				gs_vertex2f(r.x0, r.y0);
				gs_vertbuffer_t *vb = gs_render_save();
				gs_load_vertexbuffer(vb);
				gs_draw(GS_LINESTRIP, 0, 0);
				gs_vertexbuffer_destroy(vb);
			}
			if (s->debug_notrack) {
				gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFFFFFF00); // amber
				const rect_s &r = s->crop_cur;
				gs_render_start(true);
				gs_vertex2f(r.x0, r.y0);
				gs_vertex2f(r.x0, r.y1);
				gs_vertex2f(r.x1, r.y1);
				gs_vertex2f(r.x1, r.y0);
				gs_vertex2f(r.x0, r.y0);
				gs_vertbuffer_t *vb = gs_render_save();
				gs_load_vertexbuffer(vb);
				gs_draw(GS_LINESTRIP, 0, 0);
				gs_vertexbuffer_destroy(vb);
			}
		}
	}

	gs_matrix_pop();
}

static void ftf_render(void *data, gs_effect_t *)
{
	auto *s = (struct face_tracker_filter*)data;
	if (!s->target_valid) {
		obs_source_skip_video_filter(s->context);
		return;
	}
	obs_source_t *target = obs_filter_get_target(s->context);
	obs_source_t *parent = obs_filter_get_parent(s->context);

	if (!target || !parent) {
		obs_source_skip_video_filter(s->context);
		return;
	}

	if (!s->rendered) {
		render_target(s, target, parent);
		stage_to_detector(s);
		stage_to_trackers(s);
		s->rendered = true;
	}

	draw_frame(s);
}

extern "C"
void register_face_tracker_filter()
{
	struct obs_source_info info = {};
	info.id = "face_tracker_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = ftf_get_name;
	info.create = ftf_create;
	info.destroy = ftf_destroy;
	info.update = ftf_update;
	info.get_properties = ftf_properties;
	info.get_defaults = ftf_get_defaults;
	info.activate = ftf_activate,
	info.deactivate = ftf_deactivate,
	info.video_tick = ftf_tick;
	info.video_render = ftf_render;
	obs_register_source(&info);
}
