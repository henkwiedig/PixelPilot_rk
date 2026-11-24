/**
 * Device side of the OSD: the LVGL display driver on top of the DRM OSD plane and
 * the "__OSD" render thread. The widget engine itself (facts, widgets, config) lives
 * in osd.cpp and is shared with the SDL simulator, which brings its own display.
 */
#include <cassert>
#include <sys/ioctl.h>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <drm_fourcc.h>
extern "C" {
#include "drm.h"
#include "menu.h"
#include "input.h"
}
#include "osd.h"
#include "osd.hpp"
#include "osd_layer.h"
#include "spdlog/spdlog.h"
#include "../lvgl/lvgl.h"
#include "osd_gl.hpp"
#include "video_encoder.h"

int osd_zpos = 2;
extern pthread_mutex_t video_mutex;
extern pthread_cond_t video_cond;
bool osd_update_ready = false;
bool gsmenu_enabled = false;

OsdGl osd_gl;
extern bool enable_live_colortrans;
extern float live_colortrans_offset;
extern float live_colortrans_gain;

extern FrameProcessor *frame_proc;
extern bool dvr_osd;

pthread_mutex_t osd_mutex;

osd_thread_params *p;

/* LVGL renders into this cached (normal RAM) shadow buffer instead of the
 * write-combined DRM dumb buffers. Alpha blending is read-modify-write, and
 * uncached reads on ARM are extremely slow — rendering directly into the dumb
 * buffers is what made the menu sluggish. The shadow always holds the complete
 * current UI; my_flush_cb copies it to the off-screen DRM buffer and flips. */
static uint8_t * lvgl_shadow;
static uint32_t lvgl_shadow_stride;

/* An ARGB4444 OSD plane (no ARGB8888 one on this display): LVGL still renders
 * ARGB8888 into the shadow, the flush converts. Only what changed is
 * converted -- the areas of this frame, plus those of the frame before, which
 * went to the other buffer -- since a full screen is 2M pixels. The areas stay
 * separate, as LVGL hands them over: one bounding box around a timer in one
 * corner and a voltage in the other is most of the screen. */
#define OSD_MAX_AREAS 32  /* = LVGL's LV_INV_BUF_SIZE: it never hands over more */
struct osd_area_list {
	lv_area_t a[OSD_MAX_AREAS];
	int n;
};
static osd_area_list osd_dirty, osd_dirty_prev;

static void osd_area_join(lv_area_t *acc, const lv_area_t *a)
{
	if (a->x1 < acc->x1) acc->x1 = a->x1;
	if (a->y1 < acc->y1) acc->y1 = a->y1;
	if (a->x2 > acc->x2) acc->x2 = a->x2;
	if (a->y2 > acc->y2) acc->y2 = a->y2;
}

/* More areas than the list holds (a busy frame): one box around all of them. */
static void osd_area_add(osd_area_list *l, const lv_area_t *a)
{
	if (l->n < OSD_MAX_AREAS) {
		l->a[l->n++] = *a;
		return;
	}
	for (int i = 1; i < l->n; i++)
		osd_area_join(&l->a[0], &l->a[i]);
	osd_area_join(&l->a[0], a);
	l->n = 1;
}

static bool osd_area_in(const lv_area_t *in, const osd_area_list *l)
{
	for (int i = 0; i < l->n; i++)
		if (in->x1 >= l->a[i].x1 && in->y1 >= l->a[i].y1 &&
		    in->x2 <= l->a[i].x2 && in->y2 <= l->a[i].y2)
			return true;
	return false;
}

/* What the DVR re-encoder burns in (osd_layer.h): the front buffer and the
 * parts of it that are not transparent, kept in 32x32 tiles. A tile is
 * looked at again when a flushed area touches it, so the map follows the
 * shadow (= the front buffer after the flush) at the cost of the changed
 * tiles only. The blend engine pays per rectangle and per pixel, so the
 * used tiles go out as rectangles: runs along a tile row, joined with the
 * same run in the rows below. */
#define OSD_TILE 32
#define OSD_MAX_REGIONS 256
static std::vector<uint8_t> osd_tile_used, osd_tile_dirty;
static uint32_t osd_tiles_w, osd_tiles_h;
static std::mutex osd_layer_mtx;
static OsdLayerSnapshot osd_layer;

/* The ARGB8888 copy a scaling consumer registered (osd_layer.h), under
 * osd_layer_mtx. */
static int osd_copy_fd = -1;
static uint8_t *osd_copy_map;
static uint32_t osd_copy_stride, osd_copy_w, osd_copy_h;

/* linux/dma-buf.h: CPU access to a dma-buf a device reads. */
struct osd_dma_buf_sync {
	uint64_t flags;
};
#define OSD_DMA_BUF_SYNC_WRITE (2 << 0)
#define OSD_DMA_BUF_SYNC_START (0 << 2)
#define OSD_DMA_BUF_SYNC_END (1 << 2)
#define OSD_DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct osd_dma_buf_sync)

static void osd_copy_sync(bool start)
{
	struct osd_dma_buf_sync s = {OSD_DMA_BUF_SYNC_WRITE | (start ? OSD_DMA_BUF_SYNC_START : OSD_DMA_BUF_SYNC_END)};
	ioctl(osd_copy_fd, OSD_DMA_BUF_IOCTL_SYNC, &s);
}

/* Rows of the shadow (ARGB8888) into the copy, with osd_layer_mtx held. */
static void osd_copy_area(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
	x1 = LV_MAX(x1, 0);
	y1 = LV_MAX(y1, 0);
	x2 = LV_MIN(x2, (int32_t)osd_copy_w - 1);
	y2 = LV_MIN(y2, (int32_t)osd_copy_h - 1);
	if (x2 < x1)
		return;
	for (int32_t y = y1; y <= y2; y++)
		memcpy(osd_copy_map + (size_t)y * osd_copy_stride + (size_t)x1 * 4,
		       lvgl_shadow + (size_t)y * lvgl_shadow_stride + (size_t)x1 * 4, (size_t)(x2 - x1 + 1) * 4);
}

static void osd_tiles_mark(const lv_area_t *a, uint32_t w, uint32_t h)
{
	const int32_t x1 = LV_MAX(a->x1, 0), y1 = LV_MAX(a->y1, 0);
	const int32_t x2 = LV_MIN(a->x2, (int32_t)w - 1), y2 = LV_MIN(a->y2, (int32_t)h - 1);
	if (x1 > x2 || y1 > y2)
		return;
	for (int32_t ty = y1 / OSD_TILE; ty <= y2 / OSD_TILE; ty++)
		for (int32_t tx = x1 / OSD_TILE; tx <= x2 / OSD_TILE; tx++)
			osd_tile_dirty[ty * osd_tiles_w + tx] = 1;
}

/* A pixel shows when its ARGB4444 alpha does: the top nibble of the 8 bits. */
static bool osd_tile_visible(uint32_t tx, uint32_t ty, uint32_t w, uint32_t h)
{
	const uint32_t x1 = tx * OSD_TILE, y1 = ty * OSD_TILE;
	const uint32_t x2 = LV_MIN(x1 + OSD_TILE, w), y2 = LV_MIN(y1 + OSD_TILE, h);
	for (uint32_t y = y1; y < y2; y++) {
		const uint32_t *s = (const uint32_t *)(lvgl_shadow + (size_t)y * lvgl_shadow_stride) + x1;
		for (uint32_t x = x1; x < x2; x++)
			if (*s++ & 0xf0000000u)
				return true;
	}
	return false;
}

static void osd_regions_build(std::vector<OsdRegion> &out, uint32_t w, uint32_t h, bool coarse)
{
	struct Span { uint32_t x0, x1, y0, rows; bool hit; };
	std::vector<Span> open, next;
	auto emit = [&](const Span &s) {
		const uint32_t x = s.x0 * OSD_TILE, y = s.y0 * OSD_TILE;
		out.push_back({(uint16_t)x, (uint16_t)y,
		               (uint16_t)(LV_MIN(s.x1 * OSD_TILE, w) - x),
		               (uint16_t)(LV_MIN((s.y0 + s.rows) * OSD_TILE, h) - y)});
	};
	out.clear();
	for (uint32_t ty = 0; ty < osd_tiles_h; ty++) {
		const uint8_t *row = &osd_tile_used[ty * osd_tiles_w];
		next.clear();
		for (Span &s : open)
			s.hit = false;
		uint32_t tx = 0;
		while (tx < osd_tiles_w) {
			if (!row[tx]) { tx++; continue; }
			uint32_t x0 = tx;
			while (tx < osd_tiles_w && row[tx]) tx++;
			uint32_t x1 = tx;
			if (coarse) {
				/* One span per row: first to last used tile. */
				for (uint32_t t = tx; t < osd_tiles_w; t++)
					if (row[t]) x1 = t + 1;
				tx = osd_tiles_w;
			}
			bool joined = false;
			for (Span &s : open)
				if (!s.hit && s.x0 == x0 && s.x1 == x1) {
					s.rows++;
					s.hit = joined = true;
					next.push_back(s);
					break;
				}
			if (!joined)
				next.push_back({x0, x1, ty, 1, true});
		}
		for (const Span &s : open)
			if (!s.hit)
				emit(s);
		std::swap(open, next);
	}
	for (const Span &s : open)
		emit(s);
}

/* After a flush: rescan the tiles the areas touched, publish the layer. */
static void osd_layer_update(struct modeset_buf *front, const osd_area_list *areas)
{
	const uint32_t w = front->width, h = front->height;
	std::vector<OsdRegion> regions;
	if (front->format == DRM_FORMAT_ARGB4444) {
		const uint32_t tw = (w + OSD_TILE - 1) / OSD_TILE, th = (h + OSD_TILE - 1) / OSD_TILE;
		if (tw != osd_tiles_w || th != osd_tiles_h) {
			osd_tiles_w = tw;
			osd_tiles_h = th;
			osd_tile_used.assign(tw * th, 0);
			osd_tile_dirty.assign(tw * th, 1);
		}
		for (int i = 0; i < areas->n; i++)
			osd_tiles_mark(&areas->a[i], w, h);
		for (uint32_t ty = 0; ty < th; ty++)
			for (uint32_t tx = 0; tx < tw; tx++)
				if (osd_tile_dirty[ty * tw + tx]) {
					osd_tile_dirty[ty * tw + tx] = 0;
					osd_tile_used[ty * tw + tx] = osd_tile_visible(tx, ty, w, h);
				}
		osd_regions_build(regions, w, h, false);
		if (regions.size() > OSD_MAX_REGIONS)
			osd_regions_build(regions, w, h, true);
	} else {
		regions.push_back({0, 0, (uint16_t)w, (uint16_t)h});
	}

	std::lock_guard<std::mutex> lk(osd_layer_mtx);
	// The ARGB8888 copy: this frame's areas (it is one buffer, always the
	// latest, unlike the two the plane flips between).
	if (osd_copy_fd >= 0 && areas && lvgl_shadow) {
		osd_copy_sync(true);
		for (int i = 0; i < areas->n; i++)
			osd_copy_area(areas->a[i].x1, areas->a[i].y1, areas->a[i].x2, areas->a[i].y2);
		osd_copy_sync(false);
	}
	osd_layer.prime_fd = front->prime_fd;
	osd_layer.drm_format = front->format;
	osd_layer.width = w;
	osd_layer.height = h;
	osd_layer.stride = front->stride;
	osd_layer.regions.swap(regions);
	osd_layer.serial++;
	if (front->format == DRM_FORMAT_ARGB8888) {
		osd_layer.argb8888_fd = front->prime_fd;
		osd_layer.argb8888_stride = front->stride;
	} else {
		osd_layer.argb8888_fd = osd_copy_fd;
		osd_layer.argb8888_stride = osd_copy_stride;
	}
}

bool osd_layer_snapshot(OsdLayerSnapshot &out)
{
	std::lock_guard<std::mutex> lk(osd_layer_mtx);
	if (osd_layer.prime_fd < 0)
		return false;
	out = osd_layer;
	return true;
}

void osd_layer_set_argb8888_copy(int fd, uint8_t *map, uint32_t stride, uint32_t w, uint32_t h)
{
	std::lock_guard<std::mutex> lk(osd_layer_mtx);
	if (fd < 0 || !map) {
		osd_copy_fd = -1;
		osd_copy_map = nullptr;
		if (osd_layer.drm_format != DRM_FORMAT_ARGB8888)
			osd_layer.argb8888_fd = -1;
		return;
	}
	osd_copy_fd = fd;
	osd_copy_map = map;
	osd_copy_stride = stride;
	osd_copy_w = w;
	osd_copy_h = h;
	if (p && p->out) {
		osd_copy_w = LV_MIN(osd_copy_w, p->out->osd_bufs[0].width);
		osd_copy_h = LV_MIN(osd_copy_h, p->out->osd_bufs[0].height);
	}
	// What is on screen now (LVGL may be drawing into the shadow this very
	// moment; the next flush puts that area right).
	if (lvgl_shadow) {
		osd_copy_sync(true);
		osd_copy_area(0, 0, (int32_t)osd_copy_w - 1, (int32_t)osd_copy_h - 1);
		osd_copy_sync(false);
	}
	if (osd_layer.drm_format != DRM_FORMAT_ARGB8888) {
		osd_layer.argb8888_fd = fd;
		osd_layer.argb8888_stride = stride;
	}
	osd_layer.serial++;
}

static void osd_convert_argb4444(struct modeset_buf *dst, const lv_area_t *a)
{
	const int32_t x1 = LV_MAX(a->x1, 0), y1 = LV_MAX(a->y1, 0);
	const int32_t x2 = LV_MIN(a->x2, (int32_t)dst->width - 1), y2 = LV_MIN(a->y2, (int32_t)dst->height - 1);
	for (int32_t y = y1; y <= y2; y++) {
		const uint32_t *s = (const uint32_t *)(lvgl_shadow + (size_t)y * lvgl_shadow_stride) + x1;
		uint16_t *d = (uint16_t *)(dst->map + (size_t)y * dst->stride) + x1;
		for (int32_t x = x1; x <= x2; x++) {
			const uint32_t px = *s++;
			*d++ = ((px >> 16) & 0xf000) | ((px >> 12) & 0x0f00) | ((px >> 8) & 0x00f0) | ((px >> 4) & 0x000f);
		}
	}
}

void my_flush_cb(lv_display_t * display, const lv_area_t * area, uint8_t * px_map)
{
	(void)px_map;

	const bool argb4444 = p->out->osd_bufs[0].format == DRM_FORMAT_ARGB4444;
	if (argb4444)
		osd_area_add(&osd_dirty, area);

	/* Direct mode calls flush once per invalidated area. Acting on the
	 * intermediate calls used to flip the visible buffer mid-frame — the
	 * display thread commits on every video frame, so scanout could catch a
	 * half-rendered frame (rows blinking out during navigation, stale fade
	 * ghosts). Only publish once the frame is complete. */
	if (!lv_display_flush_is_last(display)) {
		lv_display_flush_ready(display);
		return;
	}

	/* osd_buf_switch is only ever flipped on this thread, so the back buffer
	 * is stable: copy without the lock. */
	int back = p->out->osd_buf_switch ^ 1;
	struct modeset_buf *dst = &p->out->osd_bufs[back];
	if (argb4444) {
		for (int i = 0; i < osd_dirty.n; i++)
			osd_convert_argb4444(dst, &osd_dirty.a[i]);
		/* The previous frame's areas, unless this frame's cover them again
		 * (the same few values changing frame after frame). */
		for (int i = 0; i < osd_dirty_prev.n; i++)
			if (!osd_area_in(&osd_dirty_prev.a[i], &osd_dirty))
				osd_convert_argb4444(dst, &osd_dirty_prev.a[i]);
		osd_layer_update(dst, &osd_dirty);
		osd_dirty_prev = osd_dirty;
		osd_dirty.n = 0;
	} else {
		memcpy(dst->map, lvgl_shadow, dst->size);
		osd_layer_update(dst, nullptr);
	}

	int ret = pthread_mutex_lock(&osd_mutex);
	assert(!ret);
	p->out->osd_buf_switch = back;
	if (enable_live_colortrans) {
		dst->gl_fb_id = osd_gl_process(dst);
	}
	ret = pthread_mutex_unlock(&osd_mutex);
	assert(!ret);

	if (dvr_osd && frame_proc)
		frame_proc->set_osd_blend(dst->prime_fd, dst->width, dst->height,
		                         dst->stride / 4);

	// tell the display thread that we have a update
	ret = pthread_mutex_lock(&video_mutex);
	assert(!ret);
	osd_update_ready = true;
	ret = pthread_cond_signal(&video_cond);
	assert(!ret);
	ret = pthread_mutex_unlock(&video_mutex);
	assert(!ret);

    /* IMPORTANT!!!
     * Inform LVGL that flushing is complete so buffer can be modified again. */
    lv_display_flush_ready(display);
}

uint32_t my_get_milliseconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

lv_display_t * display;
static lv_draw_buf_t lvgl_draw_buf1;

void setup_lvgl(osd_thread_params *p) {

	/* Initialize LVGL. */
    lv_init();

	struct modeset_buf *buf1 = &p->out->osd_bufs[0];

	display = lv_display_create(buf1->width, buf1->height);
	lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);

	/* Single cached shadow buffer as the one draw buffer (see my_flush_cb).
	 * With one buffer LVGL renders in place and never runs its dual-buffer
	 * dirty-area sync (which read back from uncached DRM memory every frame).
	 * Keep the DRM pitch (buf->stride) so the shadow's row layout matches the
	 * hardware-aligned scanline stride and the flush copy is one flat memcpy.
	 * An ARGB4444 plane's rows are half that: the shadow keeps 4 bytes a
	 * pixel there and the flush converts. */
	lvgl_shadow_stride = buf1->format == DRM_FORMAT_ARGB4444 ? buf1->stride * 2 : buf1->stride;
	const size_t shadow_used = (size_t)lvgl_shadow_stride * buf1->height;
	size_t shadow_sz = (shadow_used + 63) & ~(size_t)63;
	lvgl_shadow = (uint8_t *)aligned_alloc(64, shadow_sz);
	assert(lvgl_shadow);
	memset(lvgl_shadow, 0, shadow_sz);
	lv_draw_buf_init(&lvgl_draw_buf1, buf1->width, buf1->height,
	                 LV_COLOR_FORMAT_ARGB8888, lvgl_shadow_stride, lvgl_shadow, shadow_used);
	lv_display_set_draw_buffers(display, &lvgl_draw_buf1, NULL);
	lv_display_set_render_mode(display, LV_DISPLAY_RENDER_MODE_DIRECT);

	lv_display_set_flush_cb(display, my_flush_cb);

	lv_tick_set_cb(my_get_milliseconds);

    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_TRANSP, LV_PART_MAIN);

}

void *__OSD_THREAD__(void *param) {
	p = (osd_thread_params *)param;
	pthread_setname_np(pthread_self(), "__OSD");

	int ret = pthread_mutex_init(&osd_mutex, NULL);
	assert(!ret);

	struct modeset_buf *buf = &p->out->osd_bufs[p->out->osd_buf_switch];
	ret = modeset_perform_modeset(p->fd, p->out, p->out->osd_request, &p->out->osd_plane,
								  buf->fb, buf->width, buf->height, osd_zpos);

	if (!osd_gl.init(p->fd, buf->width, buf->height,
						live_colortrans_gain, live_colortrans_offset)) {
		spdlog::warn("OSD GL: init failed");
	}

	// LVGL is always initialized: OSD rendering now uses LVGL widgets
	setup_lvgl(p);

	// The fact processor thread starts inside osd_engine_start and blocks on the
	// lock until the widgets and the menu are built.
	osd_lock();
	// Widgets go on layer_bottom, below every screen (OSD and menu)
	osd_engine_start(lv_layer_bottom(), buf->width, buf->height,
	                 p->config_path.c_str(), nullptr);

	// Force an initial render so the video thread receives an OSD buffer signal
	lv_task_handler();

	if (gsmenu_enabled) {
		pp_menu_main();
	}
	osd_unlock();

	// Main loop: focus purely on rendering
	while (!osd_thread_signal) {

		if (gsmenu_enabled) {
			handle_keyboard_input();
		}

		osd_lock();
		// Update time-based widgets at fixed intervals
		osd_tick_if_due();
		// Render frequently for smooth animations (~60Hz)
		lv_task_handler();
		osd_unlock();

		// Sleep briefly to avoid busy loop, ~60Hz
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

	spdlog::info("OSD thread done.");
	return nullptr;
}

extern "C" uint32_t osd_gl_process(struct modeset_buf* buf){
	return osd_gl.process(buf);
}
