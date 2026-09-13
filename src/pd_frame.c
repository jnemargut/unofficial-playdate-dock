#include <string.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pd_frame.h"
#include "pd_font.h"

uint32_t pd_framebuf[FB_MAX_BYTES / 4] __attribute__((aligned(4)));

volatile bool pd_bit_reverse = true;
volatile bool pd_invert      = false;

pd_mode_t pd_mode = PD_MODE_800;
int pd_fb_w = 800, pd_fb_h = 480, pd_fb_row_bytes = 100;
int pd_scale_w = 800, pd_scale_h = 480, pd_x_byte_off = 0, pd_y_off = 0;

bool pd_scanout_half = false;

// Recompute every derived dimension from (mode, half). Called by both of the
// setters below so there is exactly one place the geometry is decided.
static void apply_geometry(void) {
	if (pd_mode == PD_MODE_800) {
		pd_fb_w = 800;
		pd_scale_w = 800;                        // exact 2x horizontally
		// Vertically the 2x is the hardware's job when half scanout is on: the
		// DVI engine emits each encoded line twice, so we store 240 rows and
		// the TV still sees 480. Same picture, half the encoding, half the
		// framebuffer touched per frame.
		pd_fb_h = pd_scale_h = pd_scanout_half ? 240 : 480;
	} else {
		pd_fb_w = 640; pd_fb_h = 480;
		pd_scale_w = 640; pd_scale_h = 384;      // 1.6x, letterboxed
	}
	pd_fb_row_bytes = pd_fb_w / 8;
	pd_x_byte_off   = (pd_fb_row_bytes - (pd_scale_w / 8)) / 2;
	pd_y_off        = (pd_fb_h - pd_scale_h) / 2;
}

void pd_frame_set_mode(pd_mode_t m) {
	pd_mode = m;
	// 640 mode scales by 1.6, which cannot be expressed as a hardware line
	// double, so it always scans out every row it stores.
	if (m != PD_MODE_800) pd_scanout_half = false;
	apply_geometry();
}

// Half scanout is only ever used for the live picture. The idle screen stays on
// the full 480 rows, because halving them would halve the vertical resolution
// of the artwork and text for no gain: nothing is competing for the CPU while
// there is no Playdate attached.
bool pd_frame_set_scanout_half(bool half) {
	if (pd_mode != PD_MODE_800) return false;
	if (half == pd_scanout_half) return half;
	pd_scanout_half = half;
	apply_geometry();
	return half;
}

// Short enough to sit in a badge. The long form is for the idle screen footer.
const char *pd_mode_short(void) {
	return pd_mode == PD_MODE_800 ? "800 X 480" : "640 X 480";
}

const char *pd_mode_name(void) {
	return pd_mode == PD_MODE_800 ? "800X480  2X  FULL SCREEN"
	                              : "640X480  1.6X  LETTERBOXED";
}

// Byte-wise bit reversal, for the paths that need it on its own.
static uint8_t rev_tab[256];

// Fused tables: bit-reverse AND expansion in a single lookup, indexed by the
// raw byte straight off the wire. The Playdate is MSB-first and libdvi wants
// LSB-first, so doing them separately meant two passes over every row. Folding
// them removes a whole pass over 12,000 bytes a frame.
static uint16_t fused2x[256];        // 1 source byte -> 2 output bytes
static uint8_t  expand5to8[32];      // 5 source bits -> 1 output byte (1.6x)

static void build_tables(void) {
	for (int i = 0; i < 256; i++) {
		uint8_t n = (uint8_t)i;
		n = (uint8_t)(((n & 0x55) << 1) | ((n & 0xaa) >> 1));
		n = (uint8_t)(((n & 0x33) << 2) | ((n & 0xcc) >> 2));
		n = (uint8_t)(((n & 0x0f) << 4) | ((n & 0xf0) >> 4));
		rev_tab[i] = n;
	}

	// 2x, fused with the MSB->LSB reversal.
	for (int v = 0; v < 256; v++) {
		uint16_t out = 0;
		for (int k = 0; k < 8; k++)
			if (v & (1 << (7 - k))) out |= (uint16_t)(3u << (2 * k));
		fused2x[v] = out;
	}

	// 1.6x: 5 source pixels map to precisely 8 output pixels, so one output
	// byte is a pure function of 5 input bits. Output pixel k samples source
	// pixel (k*5)>>3: the duplication pattern 2,2,1,2,1.
	for (int v = 0; v < 32; v++) {
		uint8_t out = 0;
		for (int k = 0; k < 8; k++)
			if (v & (1 << ((k * 5) >> 3))) out |= (uint8_t)(1u << k);
		expand5to8[v] = out;
	}
}

void pd_frame_init(void) {
	build_tables();
	pd_frame_clear(false);
}

void pd_frame_clear(bool white) {
	memset(pd_framebuf, white ? 0xff : 0x00, (size_t)(pd_fb_row_bytes * pd_fb_h));
}

void pd_frame_clear_rows(int y0, int y1) {
	if (y0 < 0) y0 = 0;
	if (y1 >= pd_fb_h) y1 = pd_fb_h - 1;
	if (y1 < y0) return;
	memset((uint8_t *)pd_framebuf + y0 * pd_fb_row_bytes, 0x00,
	       (size_t)(y1 - y0 + 1) * pd_fb_row_bytes);
}

void __not_in_flash_func(pd_frame_set_row)(unsigned int row, const uint8_t *data) {
	if ((int)row >= PD_H) return;

	uint8_t line[FB_MAX_ROW_BYTES];
	const int out_bytes = pd_scale_w / 8;

	if (pd_mode == PD_MODE_800 && pd_bit_reverse && !pd_invert) {
		// Fast path: the configuration hardware confirmed is correct. One
		// lookup per source byte, straight into the output line.
		for (int i = 0; i < PD_ROW_BYTES; i++) {
			uint16_t v = fused2x[data[i]];
			line[i * 2]     = (uint8_t)(v & 0xff);
			line[i * 2 + 1] = (uint8_t)(v >> 8);
		}
	} else {
		// Normalise to LSB-first, then expand. Two bytes of slack so the 5-bit
		// window can always read a full word.
		uint8_t src[PD_ROW_BYTES + 2];
		if (pd_bit_reverse && !pd_invert) {
			for (int i = 0; i < PD_ROW_BYTES; i++) src[i] = rev_tab[data[i]];
		} else if (!pd_bit_reverse && !pd_invert) {
			memcpy(src, data, PD_ROW_BYTES);
		} else if (!pd_bit_reverse && pd_invert) {
			for (int i = 0; i < PD_ROW_BYTES; i++) src[i] = (uint8_t)~data[i];
		} else {
			for (int i = 0; i < PD_ROW_BYTES; i++) src[i] = (uint8_t)~rev_tab[data[i]];
		}
		src[PD_ROW_BYTES] = 0;
		src[PD_ROW_BYTES + 1] = 0;

		if (pd_scale_w == PD_W * 2) {
			for (int i = 0; i < PD_ROW_BYTES; i++) {
				uint16_t v = 0;
				for (int k = 0; k < 8; k++)
					if (src[i] & (1 << k)) v |= (uint16_t)(3u << (2 * k));
				line[i * 2]     = (uint8_t)(v & 0xff);
				line[i * 2 + 1] = (uint8_t)(v >> 8);
			}
		} else {
			for (int j = 0; j < out_bytes; j++) {
				int bit = j * 5;
				uint16_t w = (uint16_t)(src[bit >> 3] | (src[(bit >> 3) + 1] << 8));
				line[j] = expand5to8[(w >> (bit & 7)) & 0x1F];
			}
		}
	}

	// Vertical: source row r covers output rows floor(r*scale)..floor((r+1)*scale)-1,
	// one or two rows. Same factor as horizontal, so proportions hold.
	int y0 = (int)((row * (unsigned)pd_scale_h) / PD_H);
	int y1 = (int)(((row + 1) * (unsigned)pd_scale_h) / PD_H);
	if (y1 > pd_scale_h) y1 = pd_scale_h;

	uint8_t *fb = (uint8_t *)pd_framebuf;
	for (int y = y0; y < y1; y++)
		memcpy(&fb[(y + pd_y_off) * pd_fb_row_bytes + pd_x_byte_off], line, (size_t)out_bytes);
}

// ---------------------------------------------------------------------------
// Bring-up drawing. None of this needs a Playdate attached, which is the point:
// it lets the DVI half be proven on its own.
// ---------------------------------------------------------------------------

static inline void put_pixel(int x, int y, bool on) {
	if (x < 0 || x >= pd_fb_w || y < 0 || y >= pd_fb_h) return;
	uint8_t *p = &((uint8_t *)pd_framebuf)[y * pd_fb_row_bytes + (x >> 3)];
	// libdvi's 1bpp mode takes the LSB of each word as the leftmost pixel.
	uint8_t mask = (uint8_t)(1u << (x & 7));
	if (on) *p |= mask; else *p &= (uint8_t)~mask;
}

// Exposed for the overlay, which composites per pixel rather than in shapes.
void pd_frame_pixel(int x, int y, bool on) { put_pixel(x, y, on); }

void pd_frame_draw_text(int col, int row, const char *s) {
	int x = col * 8, y = row * 8;
	for (const char *c = s; *c; c++, x += 8) {
		const uint8_t *g = pd_font_glyph(*c);
		for (int gy = 0; gy < 8; gy++) {
			uint8_t bits = g[gy];
			for (int gx = 0; gx < 8; gx++)
				if (bits & (0x80 >> gx)) put_pixel(x + gx, y + gy, true);
		}
	}
}

void pd_frame_draw_text_centred(int row, const char *s) {
	int col = (pd_fb_w / 8 - (int)strlen(s)) / 2;
	if (col < 0) col = 0;
	pd_frame_draw_text(col, row, s);
}

void pd_frame_border(void) {
	const int x0 = pd_x_byte_off * 8, x1 = x0 + pd_scale_w - 1;
	const int y0 = pd_y_off,          y1 = y0 + pd_scale_h - 1;
	for (int x = x0; x <= x1; x++) { put_pixel(x, y0, true); put_pixel(x, y1, true); }
	for (int y = y0; y <= y1; y++) { put_pixel(x0, y, true); put_pixel(x1, y, true); }
}

// ---------------------------------------------------------------------------
// Pixel-addressed drawing for the idle screen.
// ---------------------------------------------------------------------------

void pd_frame_text_at(int x, int y, const char *s, int scale) {
	for (const char *c = s; *c; c++, x += 8 * scale) {
		const uint8_t *g = pd_font_glyph(*c);
		for (int gy = 0; gy < 8; gy++)
			for (int gx = 0; gx < 8; gx++)
				if (g[gy] & (0x80 >> gx))
					for (int sy = 0; sy < scale; sy++)
						for (int sx = 0; sx < scale; sx++)
							put_pixel(x + gx * scale + sx, y + gy * scale + sy, true);
	}
}

void pd_frame_text_centred_at(int y, const char *s, int scale) {
	int w = (int)strlen(s) * 8 * scale;
	pd_frame_text_at((pd_fb_w - w) / 2, y, s, scale);
}

void pd_frame_rect(int x, int y, int w, int h, bool fill) {
	if (fill) {
		for (int j = 0; j < h; j++)
			for (int i = 0; i < w; i++) put_pixel(x + i, y + j, true);
	} else {
		for (int i = 0; i < w; i++) { put_pixel(x + i, y, true); put_pixel(x + i, y + h - 1, true); }
		for (int j = 0; j < h; j++) { put_pixel(x, y + j, true); put_pixel(x + w - 1, y + j, true); }
	}
}

void pd_frame_round_rect(int x, int y, int w, int h, int r) {
	for (int i = r; i < w - r; i++) { put_pixel(x + i, y, true); put_pixel(x + i, y + h - 1, true); }
	for (int j = r; j < h - r; j++) { put_pixel(x, y + j, true); put_pixel(x + w - 1, y + j, true); }
	for (int a = 0; a <= r; a++) {
		int b = (int)(0.5f + sqrtf((float)(r * r - a * a)));
		put_pixel(x + r - a,         y + r - b,         true);
		put_pixel(x + w - 1 - r + a, y + r - b,         true);
		put_pixel(x + r - a,         y + h - 1 - r + b, true);
		put_pixel(x + w - 1 - r + a, y + h - 1 - r + b, true);
	}
}

void pd_frame_line(int x0, int y0, int x1, int y1) {
	int dx = x1 - x0, dy = y1 - y0;
	int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
	          ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
	if (steps == 0) { put_pixel(x0, y0, true); return; }
	for (int i = 0; i <= steps; i++)
		put_pixel(x0 + dx * i / steps, y0 + dy * i / steps, true);
}

void pd_frame_disc(int cx, int cy, int r, bool fill) {
	for (int j = -r; j <= r; j++)
		for (int i = -r; i <= r; i++) {
			int d = i * i + j * j;
			if (fill ? (d <= r * r) : (d <= r * r && d >= (r - 1) * (r - 1)))
				put_pixel(cx + i, cy + j, true);
		}
}

void pd_frame_test_pattern(void) {
	pd_frame_clear(false);

	for (int y = 8; y < 40; y++)
		for (int x = 0; x < pd_fb_w; x++) put_pixel(x, y, ((x >> 3) & 1) != 0);

	for (int y = 44; y < 76; y++)
		for (int x = 0; x < pd_fb_w; x++) put_pixel(x, y, ((x >> 5) & 1) != 0);

	// 1px checkerboard: if the DVI clock or TMDS encode is marginal, this is
	// where it shows up first, as shimmer or fringing.
	for (int y = 80; y < 112; y++)
		for (int x = 0; x < pd_fb_w; x++) put_pixel(x, y, ((x ^ y) & 1) != 0);

	for (int i = 0; i < 16; i++) {
		put_pixel(i, 0, true);                        put_pixel(0, i, true);
		put_pixel(pd_fb_w - 1 - i, 0, true);          put_pixel(pd_fb_w - 1, i, true);
		put_pixel(i, pd_fb_h - 1, true);              put_pixel(0, pd_fb_h - 1 - i, true);
		put_pixel(pd_fb_w - 1 - i, pd_fb_h - 1, true);
		put_pixel(pd_fb_w - 1, pd_fb_h - 1 - i, true);
	}
}
