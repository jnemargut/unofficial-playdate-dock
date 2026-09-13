#include <stdio.h>
#include <string.h>
#include <math.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "common_dvi_pin_configs.h"
#include "tmds_encode.h"

#include "tusb.h"

#include "pd_frame.h"
#include "pd_audio.h"
#include "pd_stream.h"
#include "pd_imu.h"
#include "pd_ui.h"
#include "pd_font.h"

// ---------------------------------------------------------------------------
// Flipper Zero Video Game Module: a Playdate dock
//
// core1: DVI. 640x480p60, 1bpp monochrome, real 480 scanlines (no line
//        doubling; that stretched the Playdate's picture to twice its height).
// core0: TinyUSB host, Playdate stream protocol, status UI.
//
// The RP2040 runs at 252 MHz here, which PicoDVI requires. That is an overclock
// (nominal is 133) but it is also exactly what Flipper's own stock firmware
// does, so the hardware is known to tolerate it.
//
// USB host works at this clock because clk_usb comes from PLL_USB and
// set_sys_clock_khz() never touches it. This is why the native controller works
// where Pico-PIO-USB cannot: PIO-USB demands clk_sys be exactly 120 or 240 MHz.
//
// Debug console is a UART on GP16/GP17 @ 115200: host mode consumes the one
// USB controller, so there is no USB serial to debug over.
// ---------------------------------------------------------------------------

// Output mode. 800x480 lets the Playdate scale by exactly 2x and fill the frame;
// 640x480 is the mode every display accepts. Selected per build target.
// Both output modes live in one binary now; which one runs is decided at boot
// from a setting that survives in the watchdog scratch registers. Switching is
// a reboot, because PicoDVI has no way to change mode while running, but a
// reboot is under a second, so it reads as a blink.
#define MODE_MAGIC 0x50444D31u   // "PDM1"

#define LED_G_PIN 21
#define LED_R_PIN 24
#define LED_B_PIN 25

static struct dvi_inst dvi0;

static uint8_t  cdc_idx        = 0xff;
static bool     cdc_ready      = false;
static uint16_t dev_vid        = 0;
static uint16_t dev_pid        = 0;

#define PLAYDATE_VID       0x1331
#define PLAYDATE_PID_DATA  0x5740   // normal mode: serial console
#define PLAYDATE_PID_STOR  0x5741   // data disk mode: no serial

// ---------------------------------------------------------------------------
// Status LED. Cheapest plug-and-play feature on the board: it answers
// "is it working?" without a screen.
// ---------------------------------------------------------------------------
static void led_set(bool r, bool g, bool b);

// Remembered so the flip gesture can flash white and then restore.
static bool led_r_state, led_g_state, led_b_state;

static void led_restore(void) {
	gpio_put(LED_R_PIN, led_r_state);
	gpio_put(LED_G_PIN, led_g_state);
	gpio_put(LED_B_PIN, led_b_state);
}

static void led_init(void) {
	gpio_init(LED_R_PIN); gpio_set_dir(LED_R_PIN, GPIO_OUT);
	gpio_init(LED_G_PIN); gpio_set_dir(LED_G_PIN, GPIO_OUT);
	gpio_init(LED_B_PIN); gpio_set_dir(LED_B_PIN, GPIO_OUT);
	led_set(false, false, false);
}

// led_set records the colour so the gesture can restore it; led_flash does not.
static void led_set(bool r, bool g, bool b) {
	led_r_state = r; led_g_state = g; led_b_state = b;
	led_restore();
}

// ---------------------------------------------------------------------------
// core1: DVI scanout
//
// Half scanout: in 800 mode the picture is an exact 2x, so instead of core1
// encoding 480 identical-in-pairs lines, it encodes 240 and the DVI engine
// emits each one twice. Pixel for pixel the same image on the TV, half the
// TMDS encoding, half the framebuffer read per frame. Only worth having while
// a picture is streaming, so it is a request from core0 rather than a fixed
// setting; the idle screen keeps all 480 rows for its artwork.
// ---------------------------------------------------------------------------
static volatile bool scanout_half_req = false;

static void __not_in_flash_func(core1_main)(void) {
	dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
	dvi_start(&dvi0);

	// Width never changes after boot, so read it once rather than per pixel.
	const int words = pd_fb_row_bytes / 4;
	const int w     = pd_fb_w;
	int  h     = pd_fb_h;
	bool half  = pd_scanout_half;

	while (true) {
		// Latch a scanout change here, at a frame boundary, and hand the same
		// change to the DVI IRQ. This loop always runs a few scanlines ahead of
		// the IRQ, so publishing here means the IRQ picks it up at the vsync
		// immediately before the frame we are about to push: producer and
		// consumer agree on the row count for every single frame. If they ever
		// disagreed by even one buffer the picture would roll and stay rolled,
		// because nothing re-syncs the queue to the vertical position.
		if (half != scanout_half_req) {
			half = scanout_half_req;
			h    = pd_fb_h;              // core0 updates this before asking
			dvi0.vertical_repeat_mask_next = half ? 1u : 0u;
		}
		for (int y = 0; y < h; ++y) {
			const uint32_t *colourbuf = &pd_framebuf[y * words];
			uint32_t *tmdsbuf;
			// Never stop feeding scanlines. If the valid queue runs dry libdvi
			// falls back to its error blocklist, which paints a solid RED
			// screen: the comment in dvi.c says so outright.
			queue_remove_blocking_u32(&dvi0.q_tmds_free, &tmdsbuf);
			tmds_encode_1bpp(colourbuf, tmdsbuf, w);
			queue_add_blocking_u32(&dvi0.q_tmds_valid, &tmdsbuf);
		}
	}
}

// ---------------------------------------------------------------------------
// Link out. Used by pd_stream.c
// ---------------------------------------------------------------------------
void pd_link_write(const char *s) {
	if (!cdc_ready || cdc_idx == 0xff) return;
	tuh_cdc_write(cdc_idx, s, (uint32_t)strlen(s));
	tuh_cdc_write_flush(cdc_idx);
}

// ---------------------------------------------------------------------------
// Motion: one accelerometer update per video frame.
//
// Verified on real hardware before this was written: `accel` is a documented
// console command, it reaches unmodified games (Dolphin Splash moved 4.48%
// against a 0.00% idle control), and injecting at 20 Hz alongside an active
// stream cost nothing, 61 frames, 20.2 fps, zero resyncs.
// ---------------------------------------------------------------------------
// Off by default. The RP2040 host carries one bulk transfer at a time, so every
// outbound accel write steals bandwidth from the inbound video, measurably:
// throughput fell from 160 KB/s to 20 KB/s with it on. Smooth video was the
// stated priority, so motion is opt-in via the debug UART ('m') rather than
// costing every frame by default.
static volatile bool motion_enabled   = false;
static uint32_t      accel_sent       = 0;
static uint32_t      watchdog_reboots = 0;

// Patched into TinyUSB's RP2040 host driver: spurious DATA_SEQ_ERRORs used to
// panic (hang the chip); now they are counted here instead.
extern volatile uint32_t hcd_rp2040_seq_errors;

// Runs inside the parser, which runs inside tuh_cdc_rx_cb. So it must do
// nothing but raise a flag.
//
// The first version read the IMU over SPI here and then called tuh_cdc_write()
// writing to a USB device from inside its own receive callback. It survived
// about 34 frames before core0 hung, which showed up on screen as the
// diagnostic counters freezing mid-stream. Same mistake the connect handshake
// had; this was the instance I missed.
static volatile bool frame_pending = false;

void pd_stream_on_frame_complete(void) {
	frame_pending = true;
	// The frame's rows have just landed, so this is the one moment the overlay
	// can be put on top of a complete picture rather than a half-updated one.
	pd_ui_draw_overlay();
}

// ---------------------------------------------------------------------------
// Flip-to-switch.
//
// There is no button, no UART adapter and nothing on the Playdate we can safely
// steal, its buttons belong to the game. But the module has an accelerometer
// that reads gravity to within 2%, so a deliberate physical gesture is the one
// control surface available: turn the board upside down and hold it.
//
// Two seconds is long enough that nothing incidental triggers it, and the LED
// turns white while the gesture is being held so it never fires as a surprise.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Gestures.
//
// Two of them, on two different axes so they can never be confused for one
// another: turn the board over to change resolution, tip it onto its edge to
// turn sound on and off. Both want a deliberate hold, because both are annoying
// to trigger by accident while plugging cables in.
//
// The LED is the feedback. White while a resolution flip is counting down, blue
// while a sound flip is, and the real state comes back the moment you let go.
// ---------------------------------------------------------------------------
#define GESTURE_ARM_US    700000     // LED lights: the gesture has been noticed
#define GESTURE_FIRE_US  2000000     // it happens

typedef enum { GEST_NONE = 0, GEST_RESIZE, GEST_SOUND } gesture_t;

// Returns which gesture, if any, has just been held long enough to fire.
static gesture_t gesture_poll(void) {
	static absolute_time_t held_since;
	static gesture_t       holding = GEST_NONE;

	if (!pd_imu_present()) return GEST_NONE;

	pd_imu_sample_t s;
	pd_imu_read(&s);

	// Comfortably past halfway over in each case, so a board resting at an
	// angle never counts as either.
	gesture_t now = GEST_NONE;
	if (s.z < -0.70f)                                   now = GEST_RESIZE;
	else if (s.x > 0.70f || s.x < -0.70f)               now = GEST_SOUND;

	if (now != holding) {
		if (holding != GEST_NONE) led_restore();
		holding = now;
		if (now != GEST_NONE) held_since = get_absolute_time();
		return GEST_NONE;
	}
	if (now == GEST_NONE) return GEST_NONE;

	int64_t held_us = absolute_time_diff_us(held_since, get_absolute_time());
	if (held_us < GESTURE_ARM_US) return GEST_NONE;

	// Set the pins directly rather than through led_set, so the real state is
	// kept and led_restore() can put it back when the board comes down.
	gpio_put(LED_R_PIN, now == GEST_RESIZE);
	gpio_put(LED_G_PIN, now == GEST_RESIZE);
	gpio_put(LED_B_PIN, 1);

	if (held_us < GESTURE_FIRE_US) return GEST_NONE;

	holding = GEST_NONE;
	led_restore();
	return now;
}

static void service_gestures(void) {
	switch (gesture_poll()) {
	case GEST_SOUND: {
		// Nothing to reboot for: the device is simply told to start or stop
		// sending audio, and the HDMI side has been carrying data islands
		// since boot either way.
		bool on = !pd_stream_audio_enabled();
		pd_stream_set_audio(on);

		// Record it now rather than only on a resize. A watchdog reboot keeps
		// the scratch registers, so without this a recovery would restore
		// whatever the setting happened to be at the last resolution change.
		watchdog_hw->scratch[2] = MODE_MAGIC;
		watchdog_hw->scratch[3] = (uint32_t)pd_mode | (on ? 2u : 0u);

		pd_ui_flash_sound(on);
		printf("[audio] %s\n", on ? "on" : "off");
		break;
	}

	case GEST_RESIZE: {
		// Save the other mode and reboot into it. Clearing the hang counter
		// matters: this is a deliberate restart, not a crash, and it must not
		// push us into the poll-mode fallback.
		pd_mode_t next = (pd_mode == PD_MODE_800) ? PD_MODE_640 : PD_MODE_800;
		watchdog_hw->scratch[2] = MODE_MAGIC;
		watchdog_hw->scratch[3] = (uint32_t)next |
		                          (pd_stream_audio_enabled() ? 2u : 0u) |
		                          4u;   // announce the new size once, over there
		watchdog_hw->scratch[0] = 0;        // not a hang; forget the count
		watchdog_hw->scratch[1] = 0;

		printf("[mode] switching to %s\n", next == PD_MODE_800 ? "800x480" : "640x480");
		pd_stream_on_disconnect();          // tell the Playdate to stop cleanly
		sleep_ms(60);
		watchdog_reboot(0, 0, 0);
		while (true) { tight_loop_contents(); }
	}

	case GEST_NONE:
	default:
		break;
	}
}

// Called from the main loop, where writing and SPI are safe.
static void service_motion(void) {
	if (!frame_pending) return;
	frame_pending = false;

	if (!motion_enabled || !pd_imu_present()) return;
	if (!pd_stream_stats.streaming) return;

	// Rate limit. The RP2040's host controller carries only one bulk or control
	// transfer at a time, so every outbound write contends with the inbound
	// video. Sending on all ~30 frames a second measurably starved the stream, 	// throughput fell from 160 KB/s to 20 KB/s and the host eventually wedged.
	//
	// A game reading tilt does not need 30 Hz. 10 Hz is smooth, and a deadband
	// means a board sitting still sends nothing at all.
	static absolute_time_t next_accel;
	static int last_x = 1 << 20, last_y, last_z;

	if (absolute_time_diff_us(get_absolute_time(), next_accel) > 0) return;

	pd_imu_sample_t s;
	pd_imu_read(&s);

	int mx = (int)(s.x * 1000.0f), my = (int)(s.y * 1000.0f), mz = (int)(s.z * 1000.0f);
	int moved = (mx - last_x) * (mx - last_x)
	          + (my - last_y) * (my - last_y)
	          + (mz - last_z) * (mz - last_z);
	if (moved < 25 * 25) return;          // ~25 mg of noise floor

	last_x = mx; last_y = my; last_z = mz;
	next_accel = make_timeout_time_ms(100);

	// The wire format the reference client uses: milli-g as plain integers.
	pd_stream_send_accel(s.x, s.y, s.z);
	accel_sent++;
}

// ---------------------------------------------------------------------------
// TinyUSB host callbacks
// ---------------------------------------------------------------------------
void tuh_cdc_mount_cb(uint8_t idx) {
	tuh_itf_info_t itf = {0};
	tuh_cdc_itf_get_info(idx, &itf);
	tuh_vid_pid_get(itf.daddr, &dev_vid, &dev_pid);

	cdc_idx   = idx;
	cdc_ready = true;

	printf("[usb] CDC mounted idx=%u  VID=%04x PID=%04x\n", idx, dev_vid, dev_pid);

	if (dev_vid == PLAYDATE_VID && dev_pid == PLAYDATE_PID_STOR) {
		printf("[usb] Playdate is in DATA DISK mode: serial is refused there.\n");
		printf("[usb] Eject it and return to normal mode.\n");
		led_set(true, true, false);   // amber: wrong mode
		return;
	}

	if (dev_vid != PLAYDATE_VID) {
		printf("[usb] Not a Playdate; ignoring.\n");
		led_set(true, false, false);
		return;
	}

	printf("[usb] Playdate detected. Starting stream.\n");
	led_set(false, true, false);      // green: streaming
	pd_frame_clear(false);
	pd_stream_on_connect();
}

void tuh_cdc_umount_cb(uint8_t idx) {
	(void)idx;
	printf("[usb] CDC unmounted\n");
	cdc_ready = false;
	cdc_idx   = 0xff;
	dev_vid = dev_pid = 0;
	pd_stream_on_disconnect();
	led_set(false, false, true);      // blue: waiting
}

void tuh_cdc_rx_cb(uint8_t idx) {
	static uint8_t rx[1024];
	uint32_t n;

	// Bounded drain. The unbounded `while (read() > 0)` version could never
	// finish while the device was pushing 160 KB/s: completed transfers keep
	// refilling the FIFO from the USB IRQ faster than the loop empties it, so
	// the callback never returns and the main loop starves.
	//
	// Anything left over is still in the FIFO and arrives on the next callback.
	for (int i = 0; i < 16; i++) {
		n = tuh_cdc_read(idx, rx, sizeof(rx));
		if (n == 0) break;
		pd_stream_feed(rx, n);
	}
}

// ---------------------------------------------------------------------------
// Status screen (shown whenever we are not streaming)
// ---------------------------------------------------------------------------
static void draw_status(void) {
	pd_ui_state_t st;
	if (!cdc_ready)                                                  st = PD_UI_NO_DEVICE;
	else if (dev_vid == PLAYDATE_VID && dev_pid == PLAYDATE_PID_STOR) st = PD_UI_DATA_DISK;
	else if (dev_vid != PLAYDATE_VID)                                 st = PD_UI_UNKNOWN;
	else                                                              st = PD_UI_CONNECTING;
	pd_ui_draw_idle(st);
	pd_ui_draw_overlay();
}

// ---------------------------------------------------------------------------
// Debug console. Bit order and polarity are the two things most likely to be
// wrong on a first run and I have no Playdate to check against, so both are
// live-switchable rather than baked in.
// ---------------------------------------------------------------------------
static void handle_debug_key(int c) {
	switch (c) {
	case 'b':
		pd_bit_reverse = !pd_bit_reverse;
		printf("[cfg] bit_reverse = %d\n", pd_bit_reverse);
		pd_stream_request_full_frame();
		break;
	case 'i':
		pd_invert = !pd_invert;
		printf("[cfg] invert = %d\n", pd_invert);
		pd_stream_request_full_frame();
		break;
	case 't':
		printf("[cfg] test pattern\n");
		pd_frame_test_pattern();
		break;
	case 'f':
		printf("[cfg] requesting full frame\n");
		pd_stream_request_full_frame();
		break;
	case 'p':
		pd_stream_set_poll_mode(!pd_stream_poll_mode());
		printf("[cfg] poll mode = %s\n", pd_stream_poll_mode() ? "on" : "off");
		break;
	case 'r':
		printf("[cfg] reconnecting stream\n");
		if (cdc_ready) { pd_stream_on_disconnect(); pd_stream_on_connect(); }
		break;
	case 'm':
		motion_enabled = !motion_enabled;
		printf("[cfg] motion = %s\n", motion_enabled ? "on" : "off");
		break;
	case 'o':
		pd_imu_cycle_orientation();
		break;
	case 'g': {
		// Print live IMU values so orientation can be calibrated by eye against
		// the debug console, instead of guessing in firmware and reflashing.
		int16_t rx, ry, rz;
		pd_imu_sample_t s;
		pd_imu_read_raw(&rx, &ry, &rz);
		pd_imu_read(&s);
		printf("[imu] raw=(%6d,%6d,%6d)  mapped=(%+.2f,%+.2f,%+.2f) g  sent=%lu\n",
		       rx, ry, rz, (double)s.x, (double)s.y, (double)s.z,
		       (unsigned long)accel_sent);
		break;
	}
	case 's':
		printf("[stat] usb_seq_errors=%lu wdt_reboots=%lu\n",
		       (unsigned long)hcd_rp2040_seq_errors,
		       (unsigned long)watchdog_reboots);
		printf("[stat] motion=%d accel_sent=%lu imu=%d\n",
		       motion_enabled, (unsigned long)accel_sent, pd_imu_present());
		printf("[stat] streaming=%d frames=%lu rows=%lu bytes=%lu resyncs=%lu bad=%lu\n",
		       pd_stream_stats.streaming,
		       (unsigned long)pd_stream_stats.frames,
		       (unsigned long)pd_stream_stats.rows,
		       (unsigned long)pd_stream_stats.bytes_in,
		       (unsigned long)pd_stream_stats.resyncs,
		       (unsigned long)pd_stream_stats.bad_headers);
		printf("[stat] sys_clk=%lu Hz  usb_clk=%lu Hz  vid=%04x pid=%04x\n",
		       (unsigned long)clock_get_hz(clk_sys),
		       (unsigned long)clock_get_hz(clk_usb), dev_vid, dev_pid);
		break;
	case '?':
		printf("keys: b=bit order  i=invert  t=test pattern  f=full frame\n"
		       "      r=reconnect  s=stats  m=motion on/off  o=cycle orientation\n"
		       "      p=poll mode (whole frames instead of deltas)\n"
		       "      g=live IMU reading  ?=help\n");
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
int main(void) {
	// Light the LED red before touching the clock. If a boot ever dies during
	// the overclock or DVI init, the difference between "dark" and "stuck on
	// red" says whether the firmware started at all, and this board has no
	// USB serial to ask.
	led_init();
	led_set(true, false, false);

	// Pick up the saved output mode before touching the clock: the clock IS
	// the mode, since PicoDVI requires sys_clk to equal the TMDS bit clock.
	pd_mode_t want = PD_MODE_800;
	bool audio_at_boot = false;
	bool announce_mode = false;
	if (watchdog_hw->scratch[2] == MODE_MAGIC) {
		want = (pd_mode_t)(watchdog_hw->scratch[3] & 1);
		// Bit 1 carries the sound setting, so flipping the resolution does not
		// silently turn sound off again.
		audio_at_boot = (watchdog_hw->scratch[3] & 2) != 0;
		// Bit 2 is a one-shot: the resize gesture reboots, so the only way to
		// show the new size is to have the reboot carry the request with it.
		if (watchdog_hw->scratch[3] & 4) {
			announce_mode = true;
			watchdog_hw->scratch[3] &= ~4u;
		}
	}
	pd_frame_set_mode(want);

	const struct dvi_timing *timing = (want == PD_MODE_800)
		? &dvi_timing_800x480p_60hz     // 295.2 MHz, exact 2x, fills the frame
		: &dvi_timing_640x480p_60hz;    // 252 MHz, 1.6x, letterboxed

	// 295.2 MHz needs more than the 1.20 V that Flipper's own firmware ships.
	vreg_set_voltage(want == PD_MODE_800 ? VREG_VOLTAGE_1_25 : VREG_VOLTAGE_1_20);
	sleep_ms(10);
	set_sys_clock_khz(timing->bit_clk_khz, true);

	// Init the UART *after* the clock change or the divisors are computed from
	// the old frequency and the baud rate comes out wrong.
	setup_default_uart();
	printf("\n\n=== Unofficial Playdate Dock ===\n");
	printf("Not made by Panic. Not endorsed by Panic.\n");
	printf("sys_clk = %lu Hz, usb_clk = %lu Hz\n",
	       (unsigned long)clock_get_hz(clk_sys),
	       (unsigned long)clock_get_hz(clk_usb));

	led_set(false, false, true);   // blue: waiting

	pd_frame_init();
	pd_frame_test_pattern();
	pd_frame_draw_text_centred(33, "PLAYDATE DOCK  STARTING UP");
	pd_frame_draw_text_centred(35, "UNOFFICIAL, AND NOT MADE BY PANIC");

	// Bus arbitration. The first version gave core1 priority, on the reasoning
	// that a display has no elasticity. But that left DMA: which carries BOTH
	// the DVI scanout and every USB transfer, at the bottom of the pile, and
	// core1 at 295 MHz is a heavy enough bus client to starve it. Prioritising
	// DMA instead covers the two genuinely real-time paths and lets the cores
	// take what is left.
	bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
	                        BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

	dvi0.timing  = timing;
	dvi0.ser_cfg = DVI_DEFAULT_SERIAL_CONFIG;   // picodvi_dvi_cfg: the VGM wiring
	dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

	// Turns on HDMI data islands, and they stay on for the life of the boot.
	// With nothing arriving they carry digital silence, which costs the same
	// as carrying samples and saves rebuilding the scanline DMA lists under a
	// live IRQ every time sound is switched on or off.
	pd_audio_init(&dvi0);
	pd_stream_set_audio(audio_at_boot);

	// A watchdog reboot can leave core1 in a state where launching it again
	// hangs in the FIFO handshake. Resetting it first makes restart reliable.
	multicore_reset_core1();
	multicore_launch_core1(core1_main);

	// Show the test pattern briefly so the DVI half is visibly proven even if
	// nothing else ever connects, but skip it when recovering from a hang, so
	// a watchdog reset is a brief glitch rather than a four-second restart.
	if (!watchdog_caused_reboot()) sleep_ms(2500);

	pd_stream_init();

	// IMU last: SPI must be initialised after the clock change, same reason as
	// the UART. The stock firmware's own imu_test already proved the wiring
	// (Chip ID 0x47, both self-tests pass), so a failure here is ours.
	pd_imu_init();

	printf("[usb] starting host stack (TinyUSB %d.%d.%d, native controller)\n",
	       TUSB_VERSION_MAJOR, TUSB_VERSION_MINOR, TUSB_VERSION_REVISION);

	// 0.21's API. Full-speed is not a limitation here: the RP2040's native
	// controller is USB 1.1 only, and the Playdate enumerates at full speed
	// anyway (confirmed in a Linux dmesg capture: "new full-speed USB device").
	const tusb_rhport_init_t host_init = {
		.role  = TUSB_ROLE_HOST,
		.speed = TUSB_SPEED_FULL,
	};
	tusb_init(BOARD_TUH_RHPORT, &host_init);

	// If the last boot ended in a hang rather than a power cycle, say so on
	// screen, silent recovery hides bugs.
	// Scratch registers survive a watchdog reset, which is the only reason this
	// works: a counter in plain RAM would be wiped on every reboot, so the
	// fallback below could never accumulate and the thing would hang-reboot
	// forever. Which is exactly what it did.
	#define WDT_MAGIC 0x50444B31u   // "PDK1"
	if (watchdog_caused_reboot() && watchdog_hw->scratch[0] == WDT_MAGIC) {
		watchdog_reboots = watchdog_hw->scratch[1] + 1;
		printf("[boot] recovered from a watchdog reset (#%lu)\n",
		       (unsigned long)watchdog_reboots);
	} else {
		watchdog_reboots = 0;
	}
	watchdog_hw->scratch[0] = WDT_MAGIC;
	watchdog_hw->scratch[1] = watchdog_reboots;

	// Two hangs is enough to stop trusting the delta stream. Poll whole frames
	// instead: slower, but it is paced request-response rather than a continuous
	// inbound torrent, which is far gentler on a host controller that shares one
	// endpoint between control and bulk.
	if (watchdog_reboots >= 2) {
		printf("[boot] %lu hangs: falling back to whole-frame polling\n",
		       (unsigned long)watchdog_reboots);
		pd_stream_set_poll_mode(true);
	}

	// Core0 must service USB, the parser and the UI. If it ever stops, reboot
	// rather than sit in front of a frozen picture, which is exactly what a
	// USB write from inside the rx callback used to cause. Max is ~8.3 s on
	// RP2040 (errata E1 halves the counter), so 3 s is comfortably inside it.
	watchdog_enable(5000, true);

	draw_status();

	// Started here rather than at the top of main, so the badge gets its full
	// life on screen instead of spending most of it behind USB enumeration.
	if (announce_mode) pd_ui_flash_label(pd_mode_short());

	absolute_time_t next_ui = make_timeout_time_ms(66);
	bool was_streaming = false;

	while (true) {
		watchdog_update();
		tuh_task();
		pd_stream_tick();
		service_motion();
		service_gestures();

		int c = getchar_timeout_us(0);
		if (c != PICO_ERROR_TIMEOUT) handle_debug_key(c);

		bool streaming = pd_stream_stats.streaming && pd_stream_stats.frames > 0;

		if (absolute_time_diff_us(get_absolute_time(), next_ui) <= 0) {
			if (!streaming) {
				// Only ours to paint while no Playdate is driving it.
				draw_status();
			}
			next_ui = make_timeout_time_ms(66);
		}

		if (streaming != was_streaming) {
			was_streaming = streaming;
			if (streaming) {
				// Hand the vertical doubling to the DVI engine while there is
				// a picture to draw, and take it back for the idle screen.
				// Geometry first, then the request: core1 reads pd_fb_h only
				// after it sees the flag change.
				pd_frame_set_scanout_half(true);
				// The stream only ever paints the picture region, so in a
				// letterboxed mode the bands above and below would keep
				// whatever the idle screen left there for the whole session.
				// Wiping the lot is cheaper than reasoning about which parts
				// get repainted, and the next frame covers it all anyway.
				pd_frame_clear(false);
				scanout_half_req = pd_scanout_half;
			} else {
				pd_frame_set_scanout_half(false);
				scanout_half_req = pd_scanout_half;
				draw_status();
			}
			printf("[stream] %s\n", streaming ? "frames arriving" : "stopped");
		}
	}
}
