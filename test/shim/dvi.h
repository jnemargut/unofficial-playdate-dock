// Host shim: just enough of libdvi for pd_audio.c to compile and be tested.
#ifndef DVI_H
#define DVI_H
#include <stdint.h>
#include "audio_ring.h"

struct dvi_timing { uint32_t bit_clk_khz; };

struct dvi_inst {
	const struct dvi_timing *timing;
	audio_ring_t audio_ring;
};

static inline uint32_t dvi_timing_get_pixel_clock(const struct dvi_timing *t) {
	return t->bit_clk_khz * 100;
}
static inline void dvi_audio_sample_buffer_set(struct dvi_inst *i, audio_sample_t *b, int n) {
	audio_ring_set(&i->audio_ring, b, (uint32_t)n);
}

// Recorded rather than acted on, so the tests can check the numbers we hand to
// the hardware. Getting CTS wrong is inaudible on a scope and obvious on a TV.
extern int shim_audio_freq, shim_audio_cts, shim_audio_n;
static inline void dvi_set_audio_freq(struct dvi_inst *i, int freq, int cts, int n) {
	(void)i; shim_audio_freq = freq; shim_audio_cts = cts; shim_audio_n = n;
}
#endif
