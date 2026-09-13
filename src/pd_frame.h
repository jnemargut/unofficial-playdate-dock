#ifndef PD_FRAME_H
#define PD_FRAME_H

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Geometry: chosen at BOOT, not at compile time.
//
// The Playdate is 400x240, 1 bit per pixel, with square pixels (5:3).
//
// Two output modes, and the choice is a genuine trade rather than a bug:
//
//   MODE_800: 800x480 @ 295.2 MHz, exact 2x. Fills the frame completely and
//     every pixel is a clean 2x2 block, so text and edges are crisp. But the
//     Playdate fakes grey by dithering, and doubling a dither halves its
//     spatial frequency: the eye starts resolving individual dots instead of
//     blending them, so greys go chunky.
//
//   MODE_640: 640x480 @ 252 MHz, 1.6x to 640x384. Letterboxed, 80% of the
//     frame. The duplication is irregular (2,2,1,2,1), which scrambles the
//     dither grid rather than cleanly doubling it, so greys often read as fine
//     noise instead of a checkerboard. The same irregularity makes text worse.
//
// Both use the SAME factor on both axes. Stretching one more than the other is
// what made everything look squashed in the first version.
//
// The framebuffer is allocated for the larger mode and the smaller one simply
// uses part of it, so switching costs no memory.
// ---------------------------------------------------------------------------
#define PD_W            400
#define PD_H            240
#define PD_ROW_BYTES    (PD_W / 8)          // 50

#define FB_MAX_W        800
#define FB_MAX_H        480
#define FB_MAX_ROW_BYTES (FB_MAX_W / 8)     // 100
#define FB_MAX_BYTES    (FB_MAX_ROW_BYTES * FB_MAX_H)   // 48,000

typedef enum {
	PD_MODE_640 = 0,    // 640x480, 1.6x, letterboxed: smoother greys
	PD_MODE_800 = 1,    // 800x480, 2x, full frame: crisper text
} pd_mode_t;

// Live geometry. Set once by pd_frame_set_mode() before anything draws.
extern pd_mode_t pd_mode;
extern int pd_fb_w, pd_fb_h, pd_fb_row_bytes;
extern int pd_scale_w, pd_scale_h, pd_x_byte_off, pd_y_off;

// True when the framebuffer holds 240 rows and the DVI engine doubles each one
// on its way out. 800 mode only, and only while a picture is streaming.
extern bool pd_scanout_half;

void pd_frame_set_mode(pd_mode_t m);
bool pd_frame_set_scanout_half(bool half);
const char *pd_mode_name(void);
const char *pd_mode_short(void);

// The framebuffer core1 scans out. uint32_t because that's what libdvi's
// tmds_encode_1bpp() consumes.
extern uint32_t pd_framebuf[FB_MAX_BYTES / 4];

// Bit order and polarity. Hardware confirmed the Playdate is MSB-first and not
// inverted, but both stay switchable so a wrong guess costs a keypress rather
// than a reflash.
extern volatile bool pd_bit_reverse;
extern volatile bool pd_invert;

void pd_frame_init(void);

// Copy one 50-byte Playdate row in, scaling it to the current mode. `row` is
// 0-based; the caller undoes the protocol's 1-based, bit-reversed numbering.
void pd_frame_set_row(unsigned int row, const uint8_t *data);

void pd_frame_clear(bool white);
void pd_frame_clear_rows(int y0, int y1);

// --- Bring-up helpers (no Playdate required) -------------------------------
void pd_frame_test_pattern(void);
void pd_frame_draw_text(int col, int row, const char *s);
void pd_frame_draw_text_centred(int row, const char *s);
void pd_frame_border(void);

// Pixel-addressed drawing, for the idle screen.
void pd_frame_text_at(int x, int y, const char *s, int scale);
void pd_frame_text_centred_at(int y, const char *s, int scale);
void pd_frame_rect(int x, int y, int w, int h, bool fill);
void pd_frame_round_rect(int x, int y, int w, int h, int r);
void pd_frame_line(int x0, int y0, int x1, int y1);
void pd_frame_disc(int cx, int cy, int r, bool fill);
void pd_frame_pixel(int x, int y, bool on);

#endif // PD_FRAME_H
