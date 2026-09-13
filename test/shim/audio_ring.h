// Host shim. A faithful copy of libdvi's audio_ring.h with the pico
// dependencies removed, so pd_audio.c is compiled as-is by the host tests
// rather than being approximated by a stub.
#ifndef AUDIO_RING_H
#define AUDIO_RING_H
#include <stdint.h>
#include <stdbool.h>

#define __dmb() ((void)0)

typedef struct audio_sample {
	int16_t channels[2];
} audio_sample_t;

typedef struct audio_ring {
	audio_sample_t   *buffer;
	uint32_t          size;
	volatile uint32_t read;
	volatile uint32_t write;
} audio_ring_t;

static inline audio_sample_t *get_buffer_top(audio_ring_t *r)    { return r->buffer; }
static inline uint32_t get_buffer_size(audio_ring_t *r)          { return r->size;   }
static inline uint32_t get_read_offset(audio_ring_t *r)          { return r->read;   }
static inline uint32_t get_write_offset(audio_ring_t *r)         { return r->write;  }
static inline audio_sample_t *get_write_pointer(audio_ring_t *r) { return r->buffer + r->write; }
static inline audio_sample_t *get_read_pointer(audio_ring_t *r)  { return r->buffer + r->read;  }
static inline void increase_write_pointer(audio_ring_t *r, uint32_t n) { r->write = (r->write + n) & (r->size - 1); }
static inline void increase_read_pointer(audio_ring_t *r, uint32_t n)  { r->read  = (r->read  + n) & (r->size - 1); }
static inline void set_write_offset(audio_ring_t *r, uint32_t v) { r->write = v; }
static inline void set_read_offset(audio_ring_t *r, uint32_t v)  { r->read  = v; }
static inline void audio_ring_set(audio_ring_t *r, audio_sample_t *b, uint32_t n) {
	r->buffer = b; r->size = n; r->read = 0; r->write = 0;
}
#endif
