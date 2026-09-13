#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pd_frame.h"
#include "pd_ui.h"
#include "pd_stream.h"
#include "pd_font.h"

static inline void dot(int x, int y) { pd_frame_rect(x, y, 1, 1, true); }

// ---------------------------------------------------------------------------
// The screen you see when there is nothing to show yet.
//
// Deliberately not a status readout. It is a little Playdate, drawn from parts,
// with a crank that turns and a waveform wandering across its screen, so the
// dock looks alive while it waits rather than looking broken.
// ---------------------------------------------------------------------------
void pd_ui_draw_idle(pd_ui_state_t state) {
	static uint32_t tick = 0;
	tick++;

	pd_frame_clear(false);

	const int cx = pd_fb_w / 2;
	const int H  = pd_fb_h;

	// "UNOFFICIAL" sits above the name rather than below it, so the whole thing
	// reads as one title. It is the first thing on screen for a reason.
	pd_frame_text_centred_at(14, "UNOFFICIAL", 1);
	pd_frame_text_centred_at(28, "PLAYDATE DOCK", 3);
	pd_frame_text_centred_at(60, "NOT MADE BY PANIC, JUST A FAN PROJECT", 1);
	pd_frame_text_centred_at(76, "A FLIPPER VIDEO GAME MODULE MOONLIGHTING", 1);

	// --- the little Playdate -------------------------------------------------
	const int bw = 150, bh = 168;
	const int bx = cx - bw / 2 - 14;      // nudged left to leave room for the crank
	const int by = 96;

	pd_frame_round_rect(bx, by, bw, bh, 12);

	// Screen, in the Playdate's own 400x240 proportions.
	const int sw = 112, sh = 67;
	const int sx = bx + (bw - sw) / 2, sy = by + 16;
	pd_frame_rect(sx - 3, sy - 3, sw + 6, sh + 6, false);

	// Something living on the little screen: a wave that slides along.
	for (int i = 0; i < sw; i++) {
		float t = (float)i * 0.15f + (float)tick * 0.18f;
		int yy = sy + sh / 2 + (int)(sinf(t) * (float)(sh / 2 - 7));
		dot(sx + i, yy);
		dot(sx + i, yy + 1);
	}

	// D-pad and buttons
	const int ctl_y = sy + sh + 46;
	const int dpx = bx + 34;
	pd_frame_rect(dpx - 4, ctl_y - 13, 9, 26, true);
	pd_frame_rect(dpx - 13, ctl_y - 4, 26, 9, true);
	pd_frame_disc(bx + bw - 30, ctl_y + 6, 8, false);
	pd_frame_disc(bx + bw - 52, ctl_y - 8, 8, false);

	// --- the crank -----------------------------------------------------------
	// Pivot sits clear of the body so the arm never sweeps across it.
	const int px = bx + bw + 22;
	const int py = by + bh / 2;
	pd_frame_rect(bx + bw, py - 3, 22, 6, true);     // hinge arm out to the pivot
	pd_frame_disc(px, py, 5, true);

	const float a = (float)tick * 0.20f;
	const int hx = px + (int)(cosf(a) * 17.0f);
	const int hy = py + (int)(sinf(a) * 17.0f);
	pd_frame_line(px, py, hx, hy);
	pd_frame_line(px + 1, py, hx + 1, hy);
	pd_frame_disc(hx, hy, 4, true);

	// --- what is going on ----------------------------------------------------
	const char *line1, *line2;
	switch (state) {
	case PD_UI_DATA_DISK:
		line1 = "THAT IS THE DATA DISK";
		line2 = "EJECT IT AND COME BACK";
		break;
	case PD_UI_UNKNOWN:
		line1 = "SOMETHING IS PLUGGED IN";
		line2 = "BUT IT IS NOT A PLAYDATE";
		break;
	case PD_UI_CONNECTING:
		line1 = "SAYING HELLO";
		line2 = "ONE MOMENT";
		break;
	case PD_UI_NO_DEVICE:
	default:
		line1 = "NO PLAYDATE YET";
		line2 = "WAKE IT UP AND PLUG IT IN";
		break;
	}

	pd_frame_text_centred_at(292, line1, 2);
	pd_frame_text_centred_at(320, line2, 1);

	// Three dots filling up and starting over, so it never looks frozen.
	const int filled = (int)((tick / 5) % 4);
	for (int i = 0; i < 3; i++)
		pd_frame_disc(cx - 18 + i * 18, 348, 4, i < filled);

	// --- footer --------------------------------------------------------------
	pd_frame_line(cx - 190, H - 64, cx + 190, H - 64);
	pd_frame_text_centred_at(H - 54, pd_mode_name(), 1);
	pd_frame_text_centred_at(H - 38, "TURN ME OVER TO RESIZE", 1);
	pd_frame_text_centred_at(H - 22, pd_stream_audio_enabled()
		? "TIP ME ON EDGE FOR QUIET"
		: "TIP ME ON EDGE FOR SOUND", 1);
}

// ---------------------------------------------------------------------------
// Badges.
//
// A small chip that appears when something changes and dissolves away. A 1 bit
// screen has no alpha, so the fade is a dither that thins out: solid, then half
// the pixels, then a quarter, then gone. At TV distance that reads as a fade
// rather than as three steps.
//
// It draws over the live picture, which means it cannot simply be left in the
// framebuffer: delta streaming only repaints rows that changed, so an overlay
// written once would be eaten away in the moving parts of the screen and stay
// put in the still parts. So it is redrawn from scratch after every frame, and
// stops being drawn when its time is up, at which point the next frame's rows
// cover it.
// ---------------------------------------------------------------------------
#define BADGE_H      40          // display lines, not framebuffer rows
#define BADGE_MS   1600
#define BADGE_PAD    14
#define BADGE_TEXT_MAX 16

typedef enum { BADGE_NONE = 0, BADGE_SOUND, BADGE_LABEL } badge_kind_t;

static badge_kind_t badge_kind  = BADGE_NONE;
static uint32_t     badge_until = 0;
static bool         badge_sound_on = false;
static char         badge_text[BADGE_TEXT_MAX + 1];
static int          badge_w = 52;

static void badge_start(void) {
	badge_until = to_ms_since_boot(get_absolute_time()) + BADGE_MS;
}

void pd_ui_flash_sound(bool on) {
	badge_sound_on = on;
	badge_kind = BADGE_SOUND;
	badge_w = 52;
	badge_start();
}

void pd_ui_flash_label(const char *s) {
	strncpy(badge_text, s, BADGE_TEXT_MAX);
	badge_text[BADGE_TEXT_MAX] = 0;
	badge_kind = BADGE_LABEL;
	// 8x8 font at double size, plus a margin either side.
	badge_w = (int)strlen(badge_text) * 16 + BADGE_PAD * 2;
	badge_start();
}

// Distance from (px,py) to the slash, which runs corner to corner across the
// badge. Used twice: once for the white stroke, once for the black gap that
// keeps it legible where it crosses the speaker.
static float slash_dist(int px, int py) {
	const float x0 = 9.0f,  y0 = 7.0f;
	const float x1 = 43.0f, y1 = 33.0f;
	const float dx = x1 - x0, dy = y1 - y0;
	float t = ((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy);
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	const float ex = x0 + t * dx - px, ey = y0 + t * dy - py;
	return sqrtf(ex * ex + ey * ey);
}

static int speaker_pixel(int x, int y) {
	// The slash goes over the top of a complete speaker rather than replacing
	// half of it. Drawing the waves only when unmuted left the muted version
	// as a diagonal line with some crumbs around it, unreadable at the size
	// this thing is meant to be.
	if (!badge_sound_on) {
		const float d = slash_dist(x, y);
		if (d < 1.6f) return 2;      // the stroke
		if (d < 3.2f) return 1;      // the gap that keeps it legible
	}

	const int sy = y - BADGE_H / 2;   // vertical distance from the middle

	// The driver box, then the cone flaring out of it.
	if (x >= 12 && x <= 17 && sy >= -4 && sy <= 3) return 2;
	if (x >= 17 && x <= 24) {
		const int h = 4 + (x - 17);
		if (sy >= -h && sy <= h - 1) return 2;
	}

	// Two arcs, in both states: they are what makes it read as a speaker.
	if (x >= 28) {
		const int ax = x - 24;
		const float d = sqrtf((float)(ax * ax + sy * sy));
		if ((d > 6.0f && d < 7.6f) || (d > 11.0f && d < 12.6f)) return 2;
	}

	return 1;
}

static int label_pixel(int x, int y) {
	const int ty = y - (BADGE_H - 16) / 2;
	if (ty < 0 || ty >= 16) return 1;
	const int tx = x - BADGE_PAD;
	if (tx < 0) return 1;
	const int idx = tx / 16;
	if (idx >= (int)strlen(badge_text)) return 1;
	const uint8_t *g = pd_font_glyph(badge_text[idx]);
	return (g[ty / 2] & (0x80 >> ((tx % 16) / 2))) ? 2 : 1;
}

// 0 nothing, 1 paper, 2 ink. Coordinates are within the badge, in display
// pixels, so the shape stays square whatever the scanout is doing vertically.
static int badge_pixel(int x, int y) {
	// Rounded corners, so it reads as a chip rather than a sticker.
	const int r = 7;
	int cx = x < r ? r : (x >= badge_w - r ? badge_w - 1 - r : x);
	int cy = y < r ? r : (y >= BADGE_H - r ? BADGE_H - 1 - r : y);
	const int ddx = x - cx, ddy = y - cy;
	const int d2 = ddx * ddx + ddy * ddy;
	if (d2 > r * r) return 0;

	// An edge in the ink colour. The chip is paper-coloured, so without this it
	// would vanish into any bright part of the picture: the icon would still be
	// readable but the badge would stop looking like an object.
	const int edge = 2;
	if (d2 > (r - edge) * (r - edge) ||
	    x < edge || x >= badge_w - edge || y < edge || y >= BADGE_H - edge)
		return 2;

	return badge_kind == BADGE_SOUND ? speaker_pixel(x, y) : label_pixel(x, y);
}

void pd_ui_draw_overlay(void) {
	if (badge_kind == BADGE_NONE) return;
	const uint32_t now = to_ms_since_boot(get_absolute_time());
	if (now >= badge_until) { badge_kind = BADGE_NONE; return; }

	const uint32_t left = badge_until - now;
	const int step = left > (BADGE_MS * 2 / 5) ? 0
	               : (left > (BADGE_MS / 5)    ? 1 : 2);

	// Top right of the picture, with a margin. In half scanout the framebuffer
	// holds 240 rows and the engine doubles them, so vertical positions and
	// sizes are halved to keep the badge square on the TV.
	const int ox = pd_x_byte_off * 8 + pd_scale_w - badge_w - 16;
	const int oy_display = 16;

	for (int y = 0; y < BADGE_H; y++) {
		const int fy = pd_y_off + (pd_scanout_half ? (oy_display + y) / 2
		                                           : (oy_display + y));
		for (int x = 0; x < badge_w; x++) {
			if (step == 1 && ((x ^ y) & 1)) continue;
			if (step == 2 && ((x & 1) || (y & 1))) continue;

			const int v = badge_pixel(x, y);
			if (v == 0) continue;
			// Paper is white and the icon is black, which stays readable over
			// far more pictures than the other way round. The Playdate's own
			// screen is mostly white, so a dark chip disappeared into it.
			pd_frame_pixel(ox + x, fy, v == 1);
		}
	}
}
