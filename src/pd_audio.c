#include <string.h>
#include "pd_audio.h"
#include "pd_frame.h"
#include "dvi.h"
#include "audio_ring.h"

volatile uint32_t pd_audio_samples = 0;
volatile uint32_t pd_audio_dropped = 0;

static audio_sample_t ring_store[PD_AUDIO_RING];
static audio_ring_t  *ring = NULL;
static bool           is_stereo = false;

// Half a sample's worth of a straggler byte. Payloads are not guaranteed to
// split on sample boundaries, so one byte can be left over between messages.
static uint8_t  pending;
static bool     have_pending = false;

// ---------------------------------------------------------------------------
// Clock regeneration.
//
// HDMI does not carry an audio clock. It sends two numbers, N and CTS, and the
// TV recovers the sample rate as 128 * f_s = pixel_clock * N / CTS. N is fixed
// by the standard at 6272 for 44100 Hz; CTS is ours to compute, and the point
// of computing it rather than looking it up is that 800x480 is not a standard
// mode and so appears in no table.
//
//   CTS = pixel_clock * N / (128 * f_s)
//
// 128 * 44100 = 5644800, and 5644800 / 6272 = 900 exactly, so CTS is just the
// pixel clock over 900. Both of our modes land on whole numbers, which is worth
// noticing: a fractional CTS is what makes cheap sinks warble.
//
//   640x480  25.20 MHz / 900 = 28000
//   800x480  29.52 MHz / 900 = 32800
// ---------------------------------------------------------------------------
#define PD_AUDIO_N 6272

void pd_audio_init(struct dvi_inst *inst) {
	memset(ring_store, 0, sizeof(ring_store));
	dvi_audio_sample_buffer_set(inst, ring_store, PD_AUDIO_RING);
	ring = &inst->audio_ring;

	const uint32_t pixel_clk = dvi_timing_get_pixel_clock(inst->timing);
	const int cts = (int)(pixel_clk / 900u);

	// This also turns data islands on, which is why it happens once at boot
	// and never again. Toggling them later would mean rebuilding the scanline
	// DMA lists underneath a running IRQ, and there is nothing to gain by it:
	// with no samples arriving the packets carry digital silence, and the cost
	// is a fixed slice of core1 either way.
	dvi_set_audio_freq(inst, PD_AUDIO_RATE, cts, PD_AUDIO_N);
}

void pd_audio_set_stereo(bool stereo) {
	is_stereo = stereo;
}

void pd_audio_reset(void) {
	if (!ring) return;
	memset(ring_store, 0, sizeof(ring_store));
	set_write_offset(ring, get_read_offset(ring));
	have_pending = false;
}

// Room left, in stereo samples. The IRQ moves `read` underneath us, so this is
// a floor rather than an exact figure, which is the safe direction.
static inline uint32_t room(void) {
	const uint32_t r = get_read_offset(ring);
	const uint32_t w = get_write_offset(ring);
	return ((r - w - 1) & (PD_AUDIO_RING - 1));
}

static inline void put(int16_t l, int16_t r) {
	audio_sample_t *p = get_write_pointer(ring);
	p->channels[0] = l;
	p->channels[1] = r;
	increase_write_pointer(ring, 1);
}

void pd_audio_push(const uint8_t *pcm, unsigned int bytes) {
	if (!ring || bytes == 0) return;

	const unsigned int bytes_per_sample = is_stereo ? 4u : 2u;
	uint32_t free_samples = room();

	// A leftover byte from the previous message joins the front of this one.
	// Only possible in mono, where a sample is two bytes and a payload could
	// in principle carry an odd number of them.
	if (have_pending && !is_stereo) {
		if (free_samples == 0) { pd_audio_dropped++; return; }
		int16_t s = (int16_t)((uint16_t)pending | ((uint16_t)pcm[0] << 8));
		put(s, s);
		pd_audio_samples++;
		free_samples--;
		pcm++; bytes--;
		have_pending = false;
	}

	const unsigned int whole = bytes / bytes_per_sample;
	unsigned int n = whole;
	if (n > free_samples) {
		pd_audio_dropped += (n - free_samples);
		n = free_samples;
	}

	if (is_stereo) {
		for (unsigned int i = 0; i < n; i++) {
			const uint8_t *s = pcm + i * 4u;
			put((int16_t)((uint16_t)s[0] | ((uint16_t)s[1] << 8)),
			    (int16_t)((uint16_t)s[2] | ((uint16_t)s[3] << 8)));
		}
	} else {
		// Mono goes to both channels. The link cannot afford stereo: it is one
		// bulk endpoint shared with the video, and two channels would cost
		// twice the bandwidth for a handheld whose own speaker is mono.
		for (unsigned int i = 0; i < n; i++) {
			const uint8_t *s = pcm + i * 2u;
			int16_t v = (int16_t)((uint16_t)s[0] | ((uint16_t)s[1] << 8));
			put(v, v);
		}
	}
	pd_audio_samples += n;

	if (!is_stereo && (bytes & 1u)) {
		pending = pcm[bytes - 1];
		have_pending = true;
	}
}

void pd_audio_push_silence(unsigned int samples) {
	if (!ring) return;
	uint32_t free_samples = room();
	if (samples > free_samples) samples = free_samples;
	for (unsigned int i = 0; i < samples; i++) put(0, 0);
	pd_audio_samples += samples;
}
