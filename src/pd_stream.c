#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pd_stream.h"
#include "pd_frame.h"
#include "pd_audio.h"

pd_stream_stats_t pd_stream_stats;

// The reference client's command strings. Note the \r\n: the public spec says \n, and
// that is one of the places it is wrong.
static const char CMD_ECHO_OFF[]   = "echo off\r\n";
static const char CMD_ENABLE[]     = "stream enable\r\n";
// Also owed on a graceful teardown: an ungraceful disconnect can wedge the
// Playdate's USB stack until it is locked and unlocked again.
static const char CMD_DISABLE[]    = "stream disable\r\n";
static const char CMD_POKE[]       = "stream poke\r\n";
static const char CMD_AUDIO_OFF[]  = "stream a-\r\n";
// Mono, not stereo. `a+` would work and sound wider, but the Playdate sends
// audio down the same single bulk endpoint as the video, so stereo costs
// 176 KB/s against a link that carries about 225 KB/s in total and leaves
// almost nothing for the picture. Mono is 88 KB/s: about a third of the frame
// rate, for a handheld whose own speaker is mono anyway.
static const char CMD_AUDIO_ON[]   = "stream am\r\n";

// Off unless asked for, because that third of the frame rate is a real cost and
// the picture is what this dock is for.
static volatile bool audio_wanted = false;
static const char CMD_SCREEN[]     = "screen\r\n";

// The reference client sends this after `stream enable`, commented "enable full frame updates
// and application commands". On current device firmware it produces no opcode
// 14 at all, so it cannot be relied on for an initial paint. Kept because it is
// harmless and may matter on other firmware versions.
__attribute__((unused))
static const char CMD_FULLFRAME[]  = "stream fullframe\r\n";

#define ROW_PAYLOAD   52    // 1 byte row number + 50 bytes pixels + 1 pad
#define FF_HEAD_BYTES 34    // uint32 timestamp + 30 byte row mask

// The `screen` command's reply is framed by this literal, then exactly 12,000
// raw bytes: verified byte-for-byte against a real device.
static const char SCREEN_MARKER[] = "~screen:\n";
#define SCREEN_MARKER_LEN 9
#define SCREEN_BYTES      12000

typedef enum {
	ST_HEADER = 0,
	ST_PAYLOAD,      // small payload, buffered
	ST_FF_HEAD,      // full-frame header
	ST_FF_ROWS,      // full-frame row data, streamed
	ST_SKIP,         // discard a payload we don't care about
	ST_SCREEN_MARK,  // hunting for the `screen` reply marker
	ST_SCREEN_DATA,  // consuming the 12,000 byte framebuffer dump
	ST_AUDIO,        // PCM payload, streamed straight to the HDMI ring
} parse_state_t;

static struct {
	parse_state_t state;
	uint8_t  hdr[4];
	uint8_t  hdr_len;
	uint8_t  opcode;
	uint8_t  arg;
	uint16_t payload_len;

	uint8_t  buf[ROW_PAYLOAD + 4];
	uint32_t aud_left;
	uint16_t got;
	uint32_t skip_left;

	// full-frame streaming
	uint8_t  ff_mask[30];
	uint16_t ff_head_got;
	int      ff_row;        // which display row we're filling
	int      ff_bit;        // scan position through the mask
	uint16_t ff_row_got;

	// `screen` dump state
	uint8_t  mark_got;
	uint16_t scr_row;
	uint8_t  scr_col;

	absolute_time_t next_poke;
	bool connected;

	// Connect handshake, driven from the main loop.
	//
	// The first version did this inline in the USB mount callback with
	// sleep_ms() between commands, and sent `stream enable` from inside the
	// parser, which runs inside the rx callback. Both are bad: blocking a USB
	// callback stalls the host task during enumeration, and writing to a device
	// from within its own receive handler is asking for trouble. Now every
	// command is issued from pd_stream_tick(), one step at a time.
	uint8_t conn;
	absolute_time_t next_step;
	absolute_time_t screen_deadline;
	absolute_time_t last_rx;
	uint8_t screen_retries;

	// Fallback: if the delta stream will not stay up, drive the picture by
	// asking for a whole framebuffer over and over. Far less efficient, 12,000
	// bytes every time instead of 52 per changed row, but it only depends on
	// the one command already proven to work, so it always produces a live
	// image. Better a slow picture than a frozen one.
	bool    poll_mode;
	uint8_t stream_failures;
} S;

enum {
	CONN_IDLE = 0,
	CONN_ECHO,
	CONN_AUTOLOCK,
	CONN_AUDIO,
	CONN_SCREEN,
	CONN_AWAIT_SCREEN,
	CONN_ENABLE,
	CONN_AUDIO_OPT,
	CONN_RUNNING,
};

static uint8_t rev8(uint8_t n) {
	n = (uint8_t)(((n & 0x55) << 1) | ((n & 0xaa) >> 1));
	n = (uint8_t)(((n & 0x33) << 2) | ((n & 0xcc) >> 2));
	n = (uint8_t)(((n & 0x0f) << 4) | ((n & 0xf0) >> 4));
	return n;
}

void pd_stream_init(void) {
	memset(&S, 0, sizeof(S));
	memset(&pd_stream_stats, 0, sizeof(pd_stream_stats));
	S.state = ST_HEADER;
}

// Mirrors the reference client's prv_is_valid_header(). This is what lets us resync after the
// Playdate echoes a command back into the middle of the binary stream.
static bool header_valid(uint8_t opcode, uint8_t arg, uint16_t sz) {
	switch (opcode) {
		case PD_OP_DEVICE_STATE:     return sz == 8;      // mask, bits, pad, float crank
		case PD_OP_FRAME_BEGIN_DEPR: return sz == 0;
		case PD_OP_FRAME_BEGIN:      return sz == 4;      // uint32 timestamp
		case PD_OP_FRAME_END:        return sz == 0;
		case PD_OP_FRAME_ROW:        return sz == ROW_PAYLOAD;
		case PD_OP_FULL_FRAME:       return sz == FF_HEAD_BYTES + (uint32_t)arg * 50;
		case PD_OP_AUDIO_FRAME:      return sz < 2048;
		case PD_OP_AUDIO_CHANGE:     return sz == 2;
		case PD_OP_AUDIO_OFFSET:     return sz == 4;

		// That validator ends with `return true` for unrecognised
		// application sub-commands, which is far too permissive: a stray 0x99
		// byte then matches any length up to 65535 and swallows the stream.
		// Measured against the real device, accepting only the three known
		// payload sizes took resync churn from 745,522 bytes down to 4.
		case PD_OP_APPLICATION:
			return sz == 0     // RESET
			    || sz == 6     // 1-bit palette: 2 x RGB
			    || sz == 48;   // 4-bit palette: 16 x RGB

		default:                     return false;
	}
}

static void begin_payload(void) {
	if (S.payload_len == 0) {
		// Zero-length messages complete immediately.
		if (S.opcode == PD_OP_FRAME_END) {
			pd_stream_stats.frames++;
			pd_stream_stats.last_frame_ms = to_ms_since_boot(get_absolute_time());
			S.state = ST_HEADER;
			S.hdr_len = 0;
			// Frame boundary: the one moment we are certain no row data is
			// mid-flight. Send outbound input here and nowhere else.
			pd_stream_on_frame_complete();
			return;
		}
		S.state = ST_HEADER;
		S.hdr_len = 0;
		return;
	}

	if (S.opcode == PD_OP_AUDIO_FRAME) {
		// Up to 2 KB, against a 56 byte staging buffer. Audio is the only
		// payload big enough to need streaming, and it is also the only one
		// that does not care about message boundaries, so it gets handed to
		// the ring in whatever chunks the USB stack happened to deliver.
		S.aud_left = S.payload_len;
		S.state = audio_wanted ? ST_AUDIO : ST_SKIP;
		if (!audio_wanted) S.skip_left = S.payload_len;
		return;
	}

	if (S.opcode == PD_OP_FULL_FRAME) {
		S.ff_head_got = 0;
		S.ff_row = 0;
		S.ff_bit = 0;
		S.ff_row_got = 0;
		S.state = ST_FF_HEAD;
		return;
	}

	if (S.payload_len <= sizeof(S.buf)) {
		S.got = 0;
		S.state = ST_PAYLOAD;
	} else {
		S.skip_left = S.payload_len;
		S.state = ST_SKIP;
	}
}

// Takes a pointer so a payload that arrived whole can be dispatched straight
// from the USB buffer, with no staging copy at all. Row messages are ~95% of
// the stream, so that one saving is most of the parser's cost.
static void __not_in_flash_func(dispatch_payload)(const uint8_t *p) {
	switch (S.opcode) {
		case PD_OP_FRAME_ROW: {
			// Row number is bit-reversed AND 1-based. Both, together. This is
			// the single most likely thing to get wrong when working from the
			// public spec, and it shows up as a scrambled picture rather than
			// an error.
			unsigned int row = rev8(p[0]);
			if (row >= 1 && row <= PD_H) {
				pd_frame_set_row(row - 1, &p[1]);
				pd_stream_stats.rows++;
			}
			break;
		}
		case PD_OP_AUDIO_CHANGE:
			// uint16 flags: bit 0 enabled, bit 1 stereo.
			pd_audio_set_stereo((p[0] & 0x02) != 0);
			break;

		case PD_OP_AUDIO_OFFSET: {
			// The Playdate telling us it skipped ahead: a gap it produced no
			// samples for. Filling it keeps picture and sound from sliding
			// apart over a long session.
			uint32_t n = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
			             ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
			if (audio_wanted && n <= PD_AUDIO_RING) pd_audio_push_silence(n);
			break;
		}

		case PD_OP_FRAME_BEGIN:
		case PD_OP_DEVICE_STATE:
		default:
			break;
	}
	S.state = ST_HEADER;
	S.hdr_len = 0;
}

void __not_in_flash_func(pd_stream_feed)(const uint8_t *buf, uint32_t len) {
	pd_stream_stats.bytes_in += len;
	S.last_rx = get_absolute_time();

	uint32_t i = 0;
	while (i < len) {
		// Bulk-consuming states first. Byte-at-a-time dispatch was costing a
		// switch per byte at 225 KB/s; these three carry essentially all of it.
		if (S.state == ST_PAYLOAD) {
			uint32_t avail = len - i;
			if (S.got == 0 && avail >= S.payload_len) {
				// Whole payload already in hand: no staging copy needed.
				const uint8_t *p = &buf[i];
				i += S.payload_len;
				dispatch_payload(p);
			} else {
				uint32_t need = S.payload_len - S.got;
				uint32_t n = need < avail ? need : avail;
				memcpy(&S.buf[S.got], &buf[i], n);
				S.got += n;
				i += n;
				if (S.got >= S.payload_len) dispatch_payload(S.buf);
			}
			continue;
		}

		if (S.state == ST_SCREEN_DATA) {
			uint32_t avail = len - i;
			uint32_t need  = PD_ROW_BYTES - S.scr_col;
			uint32_t n     = need < avail ? need : avail;

			if (S.scr_col == 0 && avail >= PD_ROW_BYTES) {
				pd_frame_set_row(S.scr_row, &buf[i]);   // straight from USB
				i += PD_ROW_BYTES;
			} else {
				memcpy(&S.buf[S.scr_col], &buf[i], n);
				S.scr_col += n;
				i += n;
				if (S.scr_col < PD_ROW_BYTES) continue;
				pd_frame_set_row(S.scr_row, S.buf);
				S.scr_col = 0;
			}
			pd_stream_stats.rows++;

			if (++S.scr_row >= PD_H) {
				// Full screen painted. Hand back to the main loop to start
				// delta streaming, never write to the device from in here.
				pd_stream_stats.frames++;
				pd_stream_stats.streaming = true;
				S.state   = ST_HEADER;
				S.hdr_len = 0;
				S.conn      = S.poll_mode ? CONN_SCREEN : CONN_ENABLE;
				S.next_step = make_timeout_time_ms(S.poll_mode ? 80 : 250);
			}
			continue;
		}

		if (S.state == ST_AUDIO) {
			uint32_t avail = len - i;
			uint32_t n = S.aud_left < avail ? S.aud_left : avail;
			pd_audio_push(&buf[i], n);
			S.aud_left -= n;
			i += n;
			if (S.aud_left == 0) { S.state = ST_HEADER; S.hdr_len = 0; }
			continue;
		}

		if (S.state == ST_SKIP) {
			uint32_t avail = len - i;
			uint32_t n = S.skip_left < avail ? S.skip_left : avail;
			S.skip_left -= n;
			i += n;
			if (S.skip_left == 0) { S.state = ST_HEADER; S.hdr_len = 0; }
			continue;
		}

		uint8_t b = buf[i++];

		switch (S.state) {

		case ST_HEADER:
			S.hdr[S.hdr_len++] = b;
			if (S.hdr_len < 4) break;

			S.opcode      = S.hdr[0];
			S.arg         = S.hdr[1];
			S.payload_len = (uint16_t)(S.hdr[2] | (S.hdr[3] << 8));

			if (header_valid(S.opcode, S.arg, S.payload_len)) {
				begin_payload();
			} else {
				// Not a header. Slide the window one byte and try again, this
				// is how we walk out of echoed ASCII back into the binary.
				pd_stream_stats.bad_headers++;
				if (pd_stream_stats.bad_headers % 64 == 1) pd_stream_stats.resyncs++;
				S.hdr[0] = S.hdr[1];
				S.hdr[1] = S.hdr[2];
				S.hdr[2] = S.hdr[3];
				S.hdr_len = 3;
			}
			break;

		case ST_FF_HEAD:
			// 4 bytes timestamp then 30 bytes of row mask.
			if (S.ff_head_got >= 4 && S.ff_head_got < FF_HEAD_BYTES)
				S.ff_mask[S.ff_head_got - 4] = b;
			S.ff_head_got++;
			if (S.ff_head_got >= FF_HEAD_BYTES) {
				S.state = ST_FF_ROWS;
				S.ff_bit = 0;
				S.ff_row_got = 0;
				// Find the first row present in the mask.
				while (S.ff_bit < PD_H &&
				       !(S.ff_mask[S.ff_bit >> 3] & (1u << (S.ff_bit & 7))))
					S.ff_bit++;
				if (S.ff_bit >= PD_H) { S.state = ST_HEADER; S.hdr_len = 0; }
			}
			break;

		case ST_FF_ROWS:
			S.buf[S.ff_row_got++] = b;
			if (S.ff_row_got >= 50) {
				pd_frame_set_row((unsigned)S.ff_bit, S.buf);
				pd_stream_stats.rows++;
				S.ff_row_got = 0;
				S.ff_bit++;
				while (S.ff_bit < PD_H &&
				       !(S.ff_mask[S.ff_bit >> 3] & (1u << (S.ff_bit & 7))))
					S.ff_bit++;
				if (S.ff_bit >= PD_H) {
					pd_stream_stats.frames++;
					pd_stream_stats.last_frame_ms = to_ms_since_boot(get_absolute_time());
					S.state = ST_HEADER;
					S.hdr_len = 0;
				}
			}
			break;

		case ST_SCREEN_MARK:
			// Rolling match on "~screen:\n". Anything before it is the echo of
			// our own command plus a CRLF, which we simply walk past.
			if (b == (uint8_t)SCREEN_MARKER[S.mark_got]) {
				if (++S.mark_got >= SCREEN_MARKER_LEN) {
					S.state   = ST_SCREEN_DATA;
					S.scr_row = 0;
					S.scr_col = 0;
				}
			} else {
				S.mark_got = (b == (uint8_t)SCREEN_MARKER[0]) ? 1 : 0;
			}
			break;

		default:
			break;
		}
	}
}

void pd_stream_on_connect(void) {
	S.connected = true;
	S.state     = ST_HEADER;
	S.hdr_len   = 0;
	S.conn      = CONN_ECHO;
	S.next_step = get_absolute_time();
	S.last_rx   = get_absolute_time();
	S.screen_retries = 0;
	S.stream_failures = 0;
	pd_stream_stats.streaming = false;
}

// Runs from the main loop, one command per call, never from a USB callback.
static void connect_step(void) {
	absolute_time_t now = get_absolute_time();
	if (absolute_time_diff_us(now, S.next_step) > 0) return;

	switch (S.conn) {
	case CONN_ECHO:
		// Echo off FIRST. The Playdate echoes every command back and those
		// bytes land in the middle of the binary stream. The reference client's parser resyncs
		// around it; not generating it is cheaper.
		pd_link_write(CMD_ECHO_OFF);
		S.conn = CONN_AUTOLOCK;
		S.next_step = make_timeout_time_ms(40);
		break;

	case CONN_AUTOLOCK:
		// A powered Playdate mostly doesn't auto-lock, but say so explicitly, 		// and a locked Playdate leaves the USB bus entirely, which is fatal.
		pd_link_write("autolock onBattery\r\n");
		S.conn = CONN_AUDIO;
		S.next_step = make_timeout_time_ms(40);
		break;

	case CONN_AUDIO:
		// Measured on hardware: audio is on by default and costs 348 messages
		// per 5 seconds. Turning it off took the link from 22 to 33.7 fps.
		// That measurement is the whole reason this is a choice rather than a
		// setting: sound costs a third of the frame rate, every frame.
		// Always off at this point, whatever the setting is. The next step
		// pulls a whole 12,000 byte screen down the same endpoint, and there
		// is no reason to make that race audio it is about to reconfigure
		// anyway. The real setting is applied after `stream enable`, below.
		pd_link_write(CMD_AUDIO_OFF);
		S.conn = CONN_SCREEN;
		S.next_step = make_timeout_time_ms(40);
		break;

	case CONN_SCREEN:
		// Paint the whole screen once before enabling deltas. `stream enable`
		// alone resumes mid-delta and leaves most of a static screen blank, and
		// `stream fullframe` emits no opcode 14 on current device firmware.
		S.state    = ST_SCREEN_MARK;
		S.mark_got = 0;
		pd_link_write(CMD_SCREEN);
		S.conn = CONN_AWAIT_SCREEN;
		S.screen_deadline = make_timeout_time_ms(3000);
		break;

	case CONN_AWAIT_SCREEN:
		// The parser flips us to CONN_ENABLE when 12,000 bytes have landed.
		if (absolute_time_diff_us(now, S.screen_deadline) <= 0) {
			if (++S.screen_retries <= 3) {
				S.conn = CONN_SCREEN;
				S.next_step = make_timeout_time_ms(100);
			} else if (S.poll_mode) {
				// Keep trying; polling is all we have in this mode.
				S.screen_retries = 0;
				S.conn = CONN_SCREEN;
				S.next_step = make_timeout_time_ms(200);
			} else {
				// Give up on the pretty first paint and just stream.
				S.state = ST_HEADER;
				S.hdr_len = 0;
				S.conn = CONN_ENABLE;
				S.next_step = now;
			}
		}
		break;

	case CONN_ENABLE:
		pd_link_write(CMD_ENABLE);
		pd_stream_stats.streaming = true;
		S.conn = CONN_AUDIO_OPT;
		S.next_step = make_timeout_time_ms(120);
		break;

	case CONN_AUDIO_OPT:
		// Audio has to be asked for AFTER the stream is enabled, not before.
		// `stream enable` resets the device's audio configuration, so anything
		// set during the handshake is quietly discarded: the dock believed it
		// had sound and said so, and the device had never been told. The
		// reference client does the same thing in streamStarted() and that is
		// where this was eventually read from rather than guessed.
		//
		// Turning sound on with the gesture always worked precisely because
		// that path writes to an already-running stream.
		if (audio_wanted) pd_link_write(CMD_AUDIO_ON);
		S.conn = CONN_RUNNING;
		S.next_poke = make_timeout_time_ms(300);
		break;

	default:
		break;
	}
}

// Live toggle. Safe to call at any time: if the link is up the device is told
// straight away, and if it is not, the next connect handshake carries it.
void pd_stream_set_audio(bool on) {
	if (on == audio_wanted) return;
	audio_wanted = on;
	pd_audio_reset();
	if (pd_stream_stats.streaming)
		pd_link_write(on ? CMD_AUDIO_ON : CMD_AUDIO_OFF);
}

bool pd_stream_audio_enabled(void) { return audio_wanted; }

const char *pd_stream_conn_name(void) {
	switch (S.conn) {
	case CONN_IDLE:         return "IDLE";
	case CONN_ECHO:         return "ECHO";
	case CONN_AUTOLOCK:     return "LOCK";
	case CONN_AUDIO:        return "AUDI";
	case CONN_SCREEN:       return "SCRN";
	case CONN_AWAIT_SCREEN: return "WAIT";
	case CONN_ENABLE:       return "ENAB";
	case CONN_AUDIO_OPT:    return "SND ";
	case CONN_RUNNING:      return "RUN ";
	default:                return "????";
	}
}

uint32_t pd_stream_ms_since_rx(void) {
	if (!S.connected) return 0;
	int64_t d = absolute_time_diff_us(S.last_rx, get_absolute_time());
	return (uint32_t)(d / 1000);
}

void pd_stream_on_disconnect(void) {
	// Tell the device to stop before we drop it, if we still can. An abrupt
	// disconnect mid-stream is the documented way to wedge its USB stack.
	if (S.connected && pd_stream_stats.streaming) pd_link_write(CMD_DISABLE);
	S.connected = false;
	pd_stream_stats.streaming = false;
	S.state = ST_HEADER;
	S.hdr_len = 0;
}

void pd_stream_request_full_frame(void) {
	if (!S.connected) return;
	// Force a complete repaint by taking another `screen` dump. Doing this
	// while the delta stream is live interleaves 12,000 raw bytes with binary
	// messages, so stop streaming first, grab the frame, and the marker handler
	// restarts the stream once it completes.
	pd_link_write(CMD_DISABLE);
	pd_stream_stats.streaming = false;
	S.state    = ST_SCREEN_MARK;
	S.mark_got = 0;
	pd_link_write(CMD_SCREEN);
}

void pd_stream_tick(void) {
	if (!S.connected) return;

	// Walk the connect handshake before anything else.
	if (S.conn != CONN_RUNNING) {
		connect_step();
		// Poll mode cycles SCREEN -> AWAIT -> SCREEN forever and never reaches
		// CONN_RUNNING, so it needs its own stall check.
		if (S.poll_mode &&
		    absolute_time_diff_us(get_absolute_time(), S.last_rx) < -4000000) {
			pd_stream_stats.resyncs++;
			S.conn      = CONN_SCREEN;
			S.next_step = get_absolute_time();
			S.last_rx   = get_absolute_time();
			S.screen_retries = 0;
		}
		return;
	}

	absolute_time_t now = get_absolute_time();

	if (absolute_time_diff_us(now, S.next_poke) <= 0) {
		// Streaming stops after one second without a poke. 300 ms gives us
		// three chances to miss one before the picture dies.
		pd_link_write(CMD_POKE);
		S.next_poke = make_timeout_time_ms(700);
	}

	// Stall watchdog. A completely static Playdate screen legitimately sends
	// almost nothing, but it never sends *nothing at all*, pokes alone produce
	// traffic. Total silence means the stream died, so rebuild it rather than
	// sit in front of a frozen picture.
	if (absolute_time_diff_us(now, S.last_rx) < -2000000) {
		pd_stream_stats.resyncs++;
		pd_stream_stats.streaming = false;

		// Two failures is enough to stop believing in the delta stream. Drop to
		// polling whole frames, which is slow but has never once failed.
		if (++S.stream_failures >= 2) S.poll_mode = true;

		S.conn      = CONN_SCREEN;
		S.next_step = now;
		S.last_rx   = now;
		S.screen_retries = 0;
	}
}

void pd_stream_set_poll_mode(bool on) {
	S.poll_mode = on;
	S.stream_failures = 0;
	if (S.connected) {
		pd_stream_stats.streaming = false;
		S.conn      = CONN_SCREEN;
		S.next_step = get_absolute_time();
		S.last_rx   = get_absolute_time();
	}
}

bool pd_stream_poll_mode(void) { return S.poll_mode; }

// ---------------------------------------------------------------------------
// Outbound input injection
// ---------------------------------------------------------------------------

void pd_stream_send_accel(float x, float y, float z) {
	char buf[48];
	// The reference client's format exactly: milli-g as plain integers.
	snprintf(buf, sizeof(buf), "accel %i %i %i\r\n",
	         (int)(x * 1000.0f), (int)(y * 1000.0f), (int)(z * 1000.0f));
	pd_link_write(buf);
}

void pd_stream_send_button(int btn, bool pressed) {
	if (btn < 0 || btn > 7) return;
	char buf[16];
	snprintf(buf, sizeof(buf), "btn %c%d\r\n", pressed ? '+' : '-', btn);
	pd_link_write(buf);
}

void pd_stream_send_crank(float angle_deg) {
	char buf[32];
	snprintf(buf, sizeof(buf), "changecrank %.1f\r\n", (double)angle_deg);
	pd_link_write(buf);
}

void pd_stream_send_crank_docked(bool docked) {
	pd_link_write(docked ? "dockcrank 1\r\n" : "dockcrank 0\r\n");
}
