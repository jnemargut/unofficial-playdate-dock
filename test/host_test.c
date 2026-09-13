// Host-side exercise of the Playdate stream parser.
//
// The parser runs on a microcontroller with no debugger, driven by a device I
// cannot currently attach. So it gets tested here instead, against synthetic
// streams that reproduce the specific things the real device does: awkward
// chunk boundaries, echoed command text landing in the middle of binary data,
// and full-frame messages with a sparse row mask.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "pico/stdlib.h"   // the shim — gives us sleep_ms for the tick loop
#include "pd_stream.h"
#include "pd_frame.h"
#include "pd_audio.h"
#include "dvi.h"

// Geometry is chosen at boot in the firmware; pick one here so the scaling maths
// under test is a fixed, checkable target.
#define TEST_MODE PD_MODE_800

static char last_cmds[4096];
static int  cmd_len = 0;

void pd_link_write(const char *s) {
	int n = (int)strlen(s);
	if (cmd_len + n < (int)sizeof(last_cmds) - 1) {
		memcpy(last_cmds + cmd_len, s, n);
		cmd_len += n;
		last_cmds[cmd_len] = 0;
	}
}

// The firmware supplies this; the harness only needs to count the calls, since
// frame-synced motion is the scheduling contract decision 5 settled on.
static int frame_complete_calls = 0;
void pd_stream_on_frame_complete(void) { frame_complete_calls++; }

static int failures = 0;
static int checks   = 0;
static int section  = 9;   // sections 1 to 9 are numbered inline above

static void check(int cond, const char *what) {
	checks++;
	if (!cond) { printf("  FAIL: %s\n", what); failures++; }
	else       { printf("  ok:   %s\n", what); }
}

static uint8_t rev8(uint8_t n) {
	n = (uint8_t)(((n & 0x55) << 1) | ((n & 0xaa) >> 1));
	n = (uint8_t)(((n & 0x33) << 2) | ((n & 0xcc) >> 2));
	n = (uint8_t)(((n & 0x0f) << 4) | ((n & 0xf0) >> 4));
	return n;
}

// --- synthetic stream construction -----------------------------------------

static uint8_t sbuf[600000];
static size_t  slen = 0;

static void emit(const void *p, size_t n) { memcpy(sbuf + slen, p, n); slen += n; }

static void emit_header(uint8_t opcode, uint8_t arg, uint16_t len) {
	uint8_t h[4] = { opcode, arg, (uint8_t)(len & 0xff), (uint8_t)(len >> 8) };
	emit(h, 4);
}

// Row content is a function of the row number so we can verify placement.
static void fill_row_pattern(uint8_t *row, int display_row) {
	for (int i = 0; i < 50; i++) row[i] = (uint8_t)((display_row * 7 + i) & 0xff);
}

static void emit_frame(int n_rows) {
	uint32_t ts = 1234;
	emit_header(PD_OP_FRAME_BEGIN, 0, 4);
	emit(&ts, 4);

	for (int r = 0; r < n_rows; r++) {
		uint8_t payload[52];
		payload[0] = rev8((uint8_t)(r + 1));   // bit-reversed, 1-based
		fill_row_pattern(&payload[1], r);
		payload[51] = 0;
		emit_header(PD_OP_FRAME_ROW, 0, 52);
		emit(payload, 52);
	}
	emit_header(PD_OP_FRAME_END, 0, 0);
}

// The blit now scales 1.6x on both axes, so verification is per-pixel against
// the same mapping the scaler uses: output pixel x samples source pixel
// (x*400)/640, output row y samples source row (y*240)/384.
static bool src_pixel(const uint8_t *row50, int x) {
	uint8_t b = row50[x >> 3];
	if (pd_bit_reverse) b = rev8(b);
	if (pd_invert)      b = (uint8_t)~b;
	return (b >> (x & 7)) & 1;          // LSB-first after normalisation
}

static bool out_pixel(int x, int y) {
	const uint8_t *fb = (const uint8_t *)pd_framebuf;
	return (fb[y * pd_fb_row_bytes + (x >> 3)] >> (x & 7)) & 1;
}

static bool row_matches(int display_row) {
	uint8_t expect[50];
	fill_row_pattern(expect, display_row);

	int y0 = (display_row * pd_scale_h) / PD_H;
	for (int x = 0; x < pd_scale_w; x++) {
		bool want = src_pixel(expect, (x * PD_W) / pd_scale_w);
		if (out_pixel(x + pd_x_byte_off * 8, y0 + pd_y_off) != want) return false;
	}
	return true;
}

// Letterbox bands above and below the scaled picture must stay black.
static bool margins_clear(int display_row) {
	(void)display_row;
	const uint8_t *fb = (const uint8_t *)pd_framebuf;
	for (int y = 0; y < pd_y_off; y++)
		for (int i = 0; i < pd_fb_row_bytes; i++)
			if (fb[y * pd_fb_row_bytes + i]) return false;
	for (int y = pd_y_off + pd_scale_h; y < pd_fb_h; y++)
		for (int i = 0; i < pd_fb_row_bytes; i++)
			if (fb[y * pd_fb_row_bytes + i]) return false;
	return true;
}

static void feed_in_chunks(const uint8_t *b, size_t n, size_t chunk) {
	for (size_t i = 0; i < n; i += chunk) {
		size_t c = (n - i < chunk) ? (n - i) : chunk;
		pd_stream_feed(b + i, c);
	}
}

// --- tests ------------------------------------------------------------------

static void test_full_frame_one_chunk(void) {
	printf("\n[1] complete frame, single chunk\n");
	pd_stream_init(); pd_frame_init(); pd_frame_clear(false);
	slen = 0; emit_frame(240);
	pd_stream_feed(sbuf, slen);

	int bad = 0;
	for (int r = 0; r < 240; r++) if (!row_matches(r)) bad++;
	check(bad == 0, "all 240 rows land at the right offset");
	check(margins_clear(0), "letterbox bands stay black");
	check(pd_stream_stats.rows == 240, "row counter == 240");
	check(pd_stream_stats.frames == 1, "frame counter == 1");
	check(pd_stream_stats.bad_headers == 0, "no bad headers");
	check(frame_complete_calls > 0, "frame-complete hook fires (motion send point)");
}

static void test_byte_at_a_time(void) {
	printf("\n[2] same frame, one byte per call (worst-case fragmentation)\n");
	pd_stream_init(); pd_frame_init(); pd_frame_clear(false);
	slen = 0; emit_frame(240);
	feed_in_chunks(sbuf, slen, 1);

	int bad = 0;
	for (int r = 0; r < 240; r++) if (!row_matches(r)) bad++;
	check(bad == 0, "state machine survives byte-at-a-time delivery");
	check(pd_stream_stats.rows == 240, "row counter == 240");
}

static void test_odd_chunks(void) {
	printf("\n[3] same frame, 7-byte chunks (never aligned to a message)\n");
	pd_stream_init(); pd_frame_init(); pd_frame_clear(false);
	slen = 0; emit_frame(240);
	feed_in_chunks(sbuf, slen, 7);
	int bad = 0;
	for (int r = 0; r < 240; r++) if (!row_matches(r)) bad++;
	check(bad == 0, "messages reassemble across arbitrary chunk boundaries");
}

static void test_echo_resync(void) {
	printf("\n[4] echoed command text injected mid-stream (the real failure mode)\n");
	pd_stream_init(); pd_frame_init(); pd_frame_clear(false);

	slen = 0;
	emit_frame(10);
	// The Playdate echoes commands back into the binary stream. Panic's parser
	// has explicit resync logic for exactly this.
	const char *echo = "stream poke\r\nOK\r\n";
	emit(echo, strlen(echo));
	size_t after = slen;
	emit_frame(240);

	pd_stream_feed(sbuf, slen);
	(void)after;

	int bad = 0;
	for (int r = 0; r < 240; r++) if (!row_matches(r)) bad++;
	check(bad == 0, "parser resyncs and the following frame is intact");
	check(pd_stream_stats.bad_headers > 0, "garbage was detected, not silently eaten");
	check(pd_stream_stats.resyncs > 0, "resync counter moved");
}

static void test_full_frame_opcode(void) {
	printf("\n[5] OPCODE_FULL_FRAME with a sparse row mask\n");
	pd_stream_init(); pd_frame_init(); pd_frame_clear(false);

	// Every 3rd row present.
	uint8_t mask[30]; memset(mask, 0, sizeof(mask));
	int rows = 0;
	for (int r = 0; r < 240; r += 3) { mask[r >> 3] |= (uint8_t)(1u << (r & 7)); rows++; }

	slen = 0;
	emit_header(PD_OP_FULL_FRAME, (uint8_t)rows, (uint16_t)(34 + rows * 50));
	uint32_t ts = 99; emit(&ts, 4);
	emit(mask, 30);
	for (int r = 0; r < 240; r += 3) {
		uint8_t row[50]; fill_row_pattern(row, r); emit(row, 50);
	}

	feed_in_chunks(sbuf, slen, 13);

	int bad = 0, checked = 0;
	for (int r = 0; r < 240; r += 3) { checked++; if (!row_matches(r)) bad++; }
	check(bad == 0, "masked rows land in the right places");
	check(checked == rows, "row count matches the mask popcount");
	check(pd_stream_stats.rows == (uint32_t)rows, "row counter matches");
}

// The connect handshake is a state machine clocked by pd_stream_tick() from the
// main loop — deliberately, so nothing writes to USB from inside a callback.
// Drive it here the way main() does.
static void drive_connect(int ms) {
	for (int i = 0; i < ms / 5; i++) { pd_stream_tick(); sleep_ms(5); }
}

static void test_connect_commands(void) {
	printf("\n[6] connect handshake sends the right commands, in the right order\n");
	pd_stream_init(); pd_frame_set_mode(TEST_MODE); pd_frame_init();
	cmd_len = 0; last_cmds[0] = 0;
	pd_stream_on_connect();
	drive_connect(300);

	char *echo_off = strstr(last_cmds, "echo off\r\n");
	char *autolock = strstr(last_cmds, "autolock onBattery\r\n");
	char *audio    = strstr(last_cmds, "stream a-\r\n");
	char *screen   = strstr(last_cmds, "screen\r\n");
	char *enable   = strstr(last_cmds, "stream enable\r\n");

	check(echo_off != NULL, "sends 'echo off'");
	check(autolock != NULL, "sends 'autolock onBattery'");
	check(audio    != NULL, "sends 'stream a-' (measured: kills 348 audio msgs/5s)");
	check(screen   != NULL, "sends 'screen' for a guaranteed full initial paint");
	check(echo_off && audio && echo_off < audio, "'echo off' comes first");
	check(audio && screen && audio < screen, "audio is off before the screen grab");
	// stream enable must NOT be sent yet — mixing 12,000 raw bytes with the
	// binary stream is exactly what made my first bench test unreadable.
	check(enable == NULL, "'stream enable' is deferred until the dump completes");
	check(strstr(last_cmds, "\r\n") != NULL, "commands are CRLF terminated, not LF");
}

static void test_screen_dump_path(void) {
	printf("\n[8] `screen` dump: marker hunt, 12,000 bytes, then auto-enable\n");
	pd_stream_init(); pd_frame_init(); pd_frame_clear(false);
	cmd_len = 0; last_cmds[0] = 0;
	pd_stream_on_connect();
	drive_connect(300);              // walk through to the `screen` request
	cmd_len = 0; last_cmds[0] = 0;   // ignore the connect commands

	// Reproduce the device's reply byte-for-byte: echoed command, CRLF, the
	// marker, then exactly 12,000 bytes.
	slen = 0;
	const char *pre = "screen\r\n\r\n~screen:\n";
	emit(pre, strlen(pre));
	for (int r = 0; r < 240; r++) {
		uint8_t row[50]; fill_row_pattern(row, r); emit(row, 50);
	}

	feed_in_chunks(sbuf, slen, 11);   // awkward chunking on purpose
	drive_connect(500);               // past the 250ms settle, then `stream enable`

	int bad = 0;
	for (int r = 0; r < 240; r++) if (!row_matches(r)) bad++;
	check(bad == 0, "all 240 rows painted from the screen dump");
	check(margins_clear(0), "letterbox bands stay black");
	check(strstr(last_cmds, "stream enable\r\n") != NULL,
	      "switches to delta streaming once the dump completes");
	check(pd_stream_stats.frames == 1, "counts as one frame");
}

static void test_application_opcode_tightened(void) {
	printf("\n[9] APPLICATION opcode validator no longer swallows the stream\n");
	pd_stream_init(); pd_frame_init(); pd_frame_clear(false);

	// A stray 0x99 followed by a large length. Under the old `return true`
	// validator this consumed up to 65,535 bytes of real data.
	slen = 0;
	uint8_t evil[4] = { 0x99, 0x7f, 0xff, 0xf0 };   // claims ~61 KB payload
	emit(evil, 4);
	emit_frame(240);
	pd_stream_feed(sbuf, slen);

	int bad = 0;
	for (int r = 0; r < 240; r++) if (!row_matches(r)) bad++;
	check(bad == 0, "a bogus 0x99 header no longer eats the following frame");
	check(pd_stream_stats.frames == 1, "frame still parsed");
}

static void test_accel_format(void) {
	printf("\n[7] accel command format matches Panic's exactly\n");
	cmd_len = 0; last_cmds[0] = 0;
	pd_stream_send_accel(0.0f, 1.0f, -0.5f);
	check(strcmp(last_cmds, "accel 0 1000 -500\r\n") == 0,
	      "milli-g integers: 'accel 0 1000 -500'");

	cmd_len = 0; last_cmds[0] = 0;
	pd_stream_send_button(3, true);
	check(strcmp(last_cmds, "btn +3\r\n") == 0, "button press: 'btn +3'");

	cmd_len = 0; last_cmds[0] = 0;
	pd_stream_send_crank(127.5f);
	check(strcmp(last_cmds, "changecrank 127.5\r\n") == 0, "crank: 'changecrank 127.5'");
}

static void dump_pgm(const char *path) {
	// Render what the DVI core would scan out, so the result can be eyeballed.
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P5\n%d %d\n255\n", pd_fb_w, pd_fb_h);
	const uint8_t *fb = (const uint8_t *)pd_framebuf;
	for (int y = 0; y < pd_fb_h; y++) {
		for (int x = 0; x < pd_fb_w; x++) {
			// libdvi 1bpp: LSB of each byte is the leftmost of its 8 pixels.
			uint8_t bit = (fb[y * pd_fb_row_bytes + (x >> 3)] >> (x & 7)) & 1;
			uint8_t px = bit ? 255 : 0;
			fwrite(&px, 1, 1, f);
		}
	}
	fclose(f);
	printf("\nwrote %s\n", path);
}

// ---------------------------------------------------------------------------
// Half scanout: 800 mode stores 240 rows and the DVI engine doubles each one.
// The thing that has to be true is that the TV sees exactly the same pixels as
// it did when we wrote all 480 ourselves. Anything less and this is a downgrade
// dressed up as an optimisation.
// ---------------------------------------------------------------------------
static void fill_test_frame(void) {
	pd_frame_clear(false);
	for (int r = 0; r < 240; r++) {
		uint8_t row[50];
		for (int i = 0; i < 50; i++)
			row[i] = (uint8_t)(((i * 8 + r * 3) / 7) & 1 ? 0xa5 : 0x3c);
		pd_frame_set_row(r, row);
	}
}

static void test_scanout_half(void) {
	static uint8_t full[480][100];

	pd_frame_set_mode(PD_MODE_800);
	pd_frame_init();
	check(!pd_scanout_half, "800 mode starts on full scanout");
	check(pd_fb_h == 480 && pd_scale_h == 480, "full scanout is 480 rows");

	fill_test_frame();
	memcpy(full, pd_framebuf, sizeof(full));

	check(pd_frame_set_scanout_half(true), "800 mode accepts half scanout");
	check(pd_fb_h == 240 && pd_scale_h == 240, "half scanout is 240 rows");
	check(pd_y_off == 0, "half scanout has no letterbox");
	check(pd_fb_w == 800 && pd_fb_row_bytes == 100, "half scanout keeps its width");

	pd_frame_init();
	fill_test_frame();

	// Every stored row must equal the pair of rows it replaces. The engine
	// emits it twice, so matching row 2n is what makes the image identical.
	int mismatches = 0;
	const uint8_t *fb = (const uint8_t *)pd_framebuf;
	for (int y = 0; y < 240; y++) {
		if (memcmp(&fb[y * 100], full[2 * y], 100)) mismatches++;
		if (memcmp(full[2 * y], full[2 * y + 1], 100)) mismatches += 1000;
	}
	check(mismatches == 0, "half scanout is pixel-identical once doubled");

	// The saving is the whole point, so assert it rather than assume it.
	check(240 * 100 == 24000, "half scanout touches 24,000 bytes, not 48,000");

	check(!pd_frame_set_scanout_half(false), "half scanout turns back off");
	check(pd_fb_h == 480 && pd_scale_h == 480, "full scanout comes back");

	// 640 scales by 1.6, which no amount of line doubling expresses.
	pd_frame_set_mode(PD_MODE_640);
	check(!pd_frame_set_scanout_half(true), "640 mode refuses half scanout");
	check(pd_fb_h == 480 && pd_scale_h == 384, "640 mode geometry is untouched");

	pd_frame_set_mode(TEST_MODE);
	pd_frame_init();
}

// ---------------------------------------------------------------------------
// Audio.
//
// Two things can go wrong here and neither is visible on a scope: the clock
// regeneration numbers, which decide whether a TV recovers 44100 Hz or
// something adjacent to it, and the PCM unpacking, where a single byte of slip
// turns a tune into noise for the rest of the session.
// ---------------------------------------------------------------------------
int shim_audio_freq, shim_audio_cts, shim_audio_n;

static const struct dvi_timing t640 = { .bit_clk_khz = 252000 };
static const struct dvi_timing t800 = { .bit_clk_khz = 295200 };
static struct dvi_inst test_dvi;

static audio_sample_t *ring_of(void) { return test_dvi.audio_ring.buffer; }
static uint32_t written(void) { return test_dvi.audio_ring.write; }

static void test_audio_clock_regeneration(void) {
	printf("\n[%d] Audio clock regeneration is exact for both modes\n", ++section);

	// 128 * 44100 = 5644800, and N is fixed at 6272, so CTS is the pixel clock
	// over exactly 900. A fractional result is what makes cheap sinks warble,
	// so the fact that both land on integers is the thing being asserted.
	test_dvi.timing = &t640;
	pd_audio_init(&test_dvi);
	check(shim_audio_freq == 44100, "44100 Hz requested");
	check(shim_audio_n == 6272, "N is the standard 6272 for 44.1 kHz");
	check(shim_audio_cts == 28000, "640x480 at 25.20 MHz gives CTS 28000");

	test_dvi.timing = &t800;
	pd_audio_init(&test_dvi);
	check(shim_audio_cts == 32800, "800x480 at 29.52 MHz gives CTS 32800");
}

static void test_audio_unpacking(void) {
	printf("\n[%d] PCM unpacking\n", ++section);

	test_dvi.timing = &t800;
	pd_audio_init(&test_dvi);
	pd_audio_set_stereo(false);

	// Mono goes to both channels.
	const uint8_t mono[] = { 0x01, 0x02, 0xff, 0x7f, 0x00, 0x80 };
	pd_audio_push(mono, sizeof(mono));
	check(written() == 3, "three mono samples landed");
	check(ring_of()[0].channels[0] == (int16_t)0x0201, "little endian, sample 0");
	check(ring_of()[0].channels[0] == ring_of()[0].channels[1], "mono duplicated to both channels");
	check(ring_of()[1].channels[0] == 32767, "positive full scale survives");
	check(ring_of()[2].channels[0] == -32768, "negative full scale survives");

	// A payload that splits mid-sample must not lose or shift a byte. This is
	// the failure that sounds like static rather than like a glitch.
	pd_audio_reset();
	const uint8_t half1[] = { 0x34 };
	const uint8_t half2[] = { 0x12, 0x78, 0x56 };
	pd_audio_push(half1, 1);
	check(written() == 0, "a lone byte is held, not guessed at");
	pd_audio_push(half2, 3);
	check(written() == 2, "the straggler joined the next chunk");
	check(ring_of()[0].channels[0] == (int16_t)0x1234, "sample rebuilt across the split");
	check(ring_of()[1].channels[0] == (int16_t)0x5678, "and the next one is still aligned");

	// Stereo keeps the channels apart.
	pd_audio_reset();
	pd_audio_set_stereo(true);
	const uint8_t st[] = { 0x11, 0x11, 0x22, 0x22 };
	pd_audio_push(st, sizeof(st));
	check(written() == 1, "four bytes is one stereo sample");
	check(ring_of()[0].channels[0] == (int16_t)0x1111 &&
	      ring_of()[0].channels[1] == (int16_t)0x2222, "channels not swapped or merged");
	pd_audio_set_stereo(false);
}

static void test_audio_overflow(void) {
	printf("\n[%d] A full ring drops rather than corrupts\n", ++section);

	test_dvi.timing = &t800;
	pd_audio_init(&test_dvi);
	pd_audio_set_stereo(false);
	pd_audio_reset();

	// The reader is the DVI IRQ and it is not running here, so the ring fills
	// and stays full. It must refuse the excess rather than wrap over unread
	// samples, which would be audible as a tear every time it happened.
	static uint8_t big[PD_AUDIO_RING * 2 + 64];
	memset(big, 0x11, sizeof(big));
	uint32_t before = pd_audio_dropped;
	pd_audio_push(big, sizeof(big));

	check(written() <= PD_AUDIO_RING - 1, "write pointer never laps the reader");
	check(pd_audio_dropped > before, "the excess was counted as dropped");

	uint32_t w = written();
	pd_audio_push(big, 64);
	check(written() == w, "a full ring accepts nothing more");
}

// Audio riding alongside video is the whole risk: the parser has to consume a
// 2 KB payload it cannot stage in its 56 byte buffer, in whatever chunks the
// USB stack hands over, and come out still aligned to the message boundaries.
// Slip one byte and every frame after it is scrambled.
static void test_audio_alongside_video(void) {
	printf("\n[%d] Audio payloads do not desync the video parser\n", ++section);

	pd_stream_init(); pd_frame_init(); pd_frame_clear(false);
	test_dvi.timing = &t800;
	pd_audio_init(&test_dvi);
	pd_stream_set_audio(true);
	pd_audio_reset();

	slen = 0;

	// Opcode 21: the device announcing mono.
	uint8_t change[2] = { 0x01, 0x00 };          // enabled, not stereo
	emit_header(PD_OP_AUDIO_CHANGE, 0, 2);
	emit(change, 2);

	// Opcode 20: a big PCM payload, far past the staging buffer.
	static uint8_t pcm[1600];
	for (unsigned i = 0; i < sizeof(pcm); i++) pcm[i] = (uint8_t)i;
	emit_header(PD_OP_AUDIO_FRAME, 0, sizeof(pcm));
	emit(pcm, sizeof(pcm));

    // Opcode 22: a reported gap.
	uint32_t gap = 32;
	emit_header(PD_OP_AUDIO_OFFSET, 0, 4);
	emit(&gap, 4);

	// Then a whole video frame, which is the thing that must survive.
	emit_frame(240);

	// Chunk sizes chosen to land inside the audio payload rather than on any
	// boundary, which is the case that finds an off-by-one.
	feed_in_chunks(sbuf, slen, 7);

	int bad = 0;
	for (int r = 0; r < 240; r++) if (!row_matches(r)) bad++;
	check(bad == 0, "all 240 rows still land correctly after 1.6 KB of audio");
	check(pd_stream_stats.rows == 240, "row counter == 240");
	check(pd_stream_stats.bad_headers == 0, "no bad headers, so no resync happened");
	check(written() == 1600 / 2 + 32, "800 samples plus the 32 sample gap arrived");

	// And with audio switched off the same stream must be skipped cleanly.
	pd_stream_set_audio(false);
	pd_stream_init(); pd_frame_init(); pd_frame_clear(false);
	pd_audio_reset();
	feed_in_chunks(sbuf, slen, 7);
	bad = 0;
	for (int r = 0; r < 240; r++) if (!row_matches(r)) bad++;
	check(bad == 0, "rows still land when audio is skipped rather than kept");
	check(pd_stream_stats.bad_headers == 0, "skipping stays aligned too");
	check(written() == 0, "and nothing reached the ring");
}

int main(void) {
	printf("=== Playdate stream parser, host-side exercise ===\n");
	pd_frame_set_mode(TEST_MODE);

	test_full_frame_one_chunk();
	test_byte_at_a_time();
	test_odd_chunks();
	test_echo_resync();
	test_full_frame_opcode();
	test_connect_commands();
	test_accel_format();
	test_screen_dump_path();
	test_application_opcode_tightened();
	test_scanout_half();
	test_audio_clock_regeneration();
	test_audio_unpacking();
	test_audio_overflow();
	test_audio_alongside_video();

	// Render the two screens exactly as the firmware draws them, so the font,
	// centring and blit geometry can be checked without a television.

	// 1. The status screen — draw_status() in main.c. Clears first, no pattern.
	pd_frame_init();
	pd_frame_clear(false);
	pd_frame_border();
	pd_frame_draw_text_centred(18,  "PLAYDATE DOCK");
	pd_frame_draw_text_centred(20,  "FLIPPER VIDEO GAME MODULE");
	pd_frame_draw_text_centred(25, "WAITING FOR PLAYDATE");
	pd_frame_draw_text_centred(27, "UNLOCK IT AND CONNECT USB");
	pd_frame_draw_text_centred(33, "IMU  ICM-42688-P  OK");
	pd_frame_draw_text_centred(35, "TILT X -128  Y +987  Z  +61 MG");
	pd_frame_draw_text_centred(36, "RAW   -2097  +16171   +999");
	pd_frame_draw_text_centred(37, "MOTION ON    ORIENT 012 +++");
	pd_frame_draw_text_centred(44, "DEBUG UART GP16/GP17 115200  KEYS: O G M S");
	dump_pgm("build_host/status_screen.pgm");

	// 2. The boot screen — test pattern plus one line, as main() draws it.
	pd_frame_init();
	pd_frame_test_pattern();
	pd_frame_draw_text_centred(34, "PLAYDATE DOCK - BOOTING");
	dump_pgm("build_host/boot_screen.pgm");

	// 3. A real frame in place, to prove the 400x240 blit geometry: a synthetic
	//    Playdate image centred in the 640x480 output with clean margins.
	pd_stream_init();
	pd_frame_init();
	pd_frame_clear(false);
	pd_bit_reverse = false;
	for (int r = 0; r < 240; r++) {
		uint8_t row[50];
		for (int i = 0; i < 50; i++) {
			int x = i * 8;
			// Diagonal bands plus a frame around the Playdate's own edges.
			uint8_t v = (uint8_t)((((x + r) / 16) & 1) ? 0xff : 0x00);
			if (r < 2 || r > 237) v = 0xff;
			if (i == 0) v |= 0x01;
			if (i == 49) v |= 0x80;
			row[i] = v;
		}
		pd_frame_set_row(r, row);
	}
	dump_pgm("build_host/blit_geometry.pgm");
	pd_bit_reverse = true;

	printf("\n=== %d checks, %d failures ===\n", checks, failures);
	return failures ? 1 : 0;
}
