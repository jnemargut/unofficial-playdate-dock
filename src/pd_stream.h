#ifndef PD_STREAM_H
#define PD_STREAM_H

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Playdate screen-streaming protocol.
//
// Ported from the Playdate Cabinet client (daveatpanic/PlaydateCabinet,
// mirrorpi/), not
// from the public reverse-engineered spec: the spec gets the message header
// wrong. It is uint8 opcode + uint8 arg + uint16 length, NOT uint16 type +
// uint16 length. That only bites on two opcodes, which is presumably why it
// went unnoticed.
//
// Audio and palettes are deliberately not implemented; we are monochrome and
// the TV gets its sound from somewhere else.
// ---------------------------------------------------------------------------

enum {
	PD_OP_DEVICE_STATE       = 1,
	PD_OP_FRAME_BEGIN_DEPR   = 10,
	PD_OP_FRAME_END          = 11,
	PD_OP_FRAME_ROW          = 12,
	PD_OP_FRAME_BEGIN        = 13,
	PD_OP_FULL_FRAME         = 14,
	PD_OP_AUDIO_FRAME        = 20,
	PD_OP_AUDIO_CHANGE       = 21,
	PD_OP_AUDIO_OFFSET       = 22,
	PD_OP_APPLICATION        = 0x99,
};

// Supplied by main.c: writes a NUL-terminated string to the Playdate's CDC
// port. Kept as a hook so the parser has no dependency on TinyUSB.
extern void pd_link_write(const char *s);

// Called the moment an end-of-frame marker is parsed. This is the scheduling
// point decision 5 chose: the video stream's own structure clocks our outbound
// writes, so a motion update can never land between a frame's row messages, and
// the rate matches the display for free rather than needing a timer.
extern void pd_stream_on_frame_complete(void);

void pd_stream_init(void);

// Call once the CDC device is mounted and ready.
void pd_stream_on_connect(void);
void pd_stream_on_disconnect(void);

// Feed raw bytes from the CDC port. Safe to call with arbitrary chunk sizes;
// the parser keeps its own state across calls and resyncs on garbage.
void pd_stream_feed(const uint8_t *buf, uint32_t len);

// Call regularly from the main loop. Handles the keepalive: streaming stops if
// the Playdate goes more than a second without a poke.
void pd_stream_tick(void);

// Force a complete repaint. Needed after a reconnect, because a plain
// `stream enable` does NOT resend the whole screen, it picks up mid-delta.
void pd_stream_request_full_frame(void);

// --- Outbound input injection ----------------------------------------------
// These feed the Playdate's real input APIs, so they work in unmodified games.
// `accel` is the one that has never been verified in the field: it is defined
// in that client but never called, and its forward declaration is commented
// out. Treat it as unproven until the bench test says otherwise.
void pd_stream_send_accel(float x, float y, float z);   // in g
void pd_stream_send_button(int btn, bool pressed);      // btn: 0..7
void pd_stream_send_crank(float angle_deg);
void pd_stream_send_crank_docked(bool docked);

// --- Stats, for the status overlay and the debug UART -----------------------
typedef struct {
	uint32_t frames;
	uint32_t rows;
	uint32_t resyncs;
	uint32_t bad_headers;
	uint32_t bytes_in;
	uint32_t last_frame_ms;
	bool     streaming;
} pd_stream_stats_t;

extern pd_stream_stats_t pd_stream_stats;

// Where the connect handshake has got to, as a short label for the on-screen
// diagnostic strip. Host mode leaves us no USB serial, so the television is the
// only place these can be seen without a UART adapter.
const char *pd_stream_conn_name(void);

// HDMI audio. Costs about a third of the frame rate, so it is opt-in.
void pd_stream_set_audio(bool on);
bool pd_stream_audio_enabled(void);

// Fallback transport: poll whole framebuffers with `screen` instead of running
// the delta stream. Slower, but depends only on a command already proven to
// work, so it always yields a live picture.
void pd_stream_set_poll_mode(bool on);
bool pd_stream_poll_mode(void);
uint32_t    pd_stream_ms_since_rx(void);

#endif // PD_STREAM_H
