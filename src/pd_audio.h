#ifndef PD_AUDIO_H
#define PD_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Playdate audio to HDMI.
//
// The Playdate sends 16 bit signed little endian PCM at 44100 Hz over the same
// serial link as the video, and the DVI engine carries it to the TV in HDMI
// data islands: small packets squeezed into the horizontal blanking of every
// scanline, where a plain DVI signal sends nothing at all.
//
// The two ends run off different clocks and neither can be adjusted. The
// Playdate writes samples at its own 44100 Hz, the data islands drain them at
// whatever 44100 Hz our pixel clock works out to, and over a long session those
// two drift apart. The ring below absorbs the wobble; the overflow and underflow
// behaviour is what handles the drift, and both are deliberately boring. Running
// dry emits digital silence, which is inaudible. Running full drops the newest
// samples, which is a click roughly never.
// ---------------------------------------------------------------------------

#define PD_AUDIO_RATE      44100
#define PD_AUDIO_RING_LOG2 11
#define PD_AUDIO_RING      (1u << PD_AUDIO_RING_LOG2)   // 2048 stereo samples

struct dvi_inst;

// Hands the ring to libdvi and works out the clock regeneration numbers for
// whichever video mode booted. Call after dvi_init and before dvi_start.
void pd_audio_init(struct dvi_inst *inst);

// Opcode 21: the Playdate telling us how it is going to send audio.
void pd_audio_set_stereo(bool stereo);

// Opcode 20: raw PCM, exactly as it arrived off the wire.
void pd_audio_push(const uint8_t *pcm, unsigned int bytes);

// Opcode 22: the Playdate reporting a gap it did not send samples for.
void pd_audio_push_silence(unsigned int samples);

// Throw away anything buffered. Used when audio is switched off, so switching
// it back on does not replay a slice of whatever was playing before.
void pd_audio_reset(void);

// Counters, for the debug console.
extern volatile uint32_t pd_audio_samples;
extern volatile uint32_t pd_audio_dropped;

#endif // PD_AUDIO_H
