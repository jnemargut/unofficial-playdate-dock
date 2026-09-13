// Renders the real idle screen to a PGM so it can be looked at without a TV.
// Draws the actual pd_ui_draw_idle(), not a copy of it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pd_frame.h"
#include "pd_ui.h"

// The idle screen reports the sound setting in its footer, so the renderer has
// to satisfy the stream module's one external dependency. Nothing is sent here.
int shim_audio_freq, shim_audio_cts, shim_audio_n;
void pd_link_write(const char *s) { (void)s; }
void pd_stream_on_frame_complete(void) {}

static void dump(const char *path) {
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P5\n%d %d\n255\n", pd_fb_w, pd_fb_h);
	const uint8_t *fb = (const uint8_t *)pd_framebuf;
	for (int y = 0; y < pd_fb_h; y++)
		for (int x = 0; x < pd_fb_w; x++) {
			uint8_t px = ((fb[y * pd_fb_row_bytes + (x >> 3)] >> (x & 7)) & 1) ? 255 : 0;
			fwrite(&px, 1, 1, f);
		}
	fclose(f);
	printf("wrote %s (%dx%d)\n", path, pd_fb_w, pd_fb_h);
}

int main(int argc, char **argv) {
	int frames = argc > 1 ? atoi(argv[1]) : 1;
	pd_frame_set_mode(PD_MODE_800);
	pd_frame_init();

	// The screen animates off an internal tick, so draw repeatedly and keep a
	// few frames to check the motion actually reads.
	for (int i = 1; i <= frames; i++) {
		pd_ui_draw_idle(PD_UI_NO_DEVICE);
		if (i == 1 || i == frames / 2 || i == frames) {
			char path[64];
			snprintf(path, sizeof(path), "build_host/ui_%02d.pgm", i);
			dump(path);
		}
	}
	return 0;
}
