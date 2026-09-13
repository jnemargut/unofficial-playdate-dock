#ifndef PD_UI_H
#define PD_UI_H

// The idle screen. Lives here rather than in main.c so the host-side renderer
// can draw the real thing and it can be looked at before it reaches a TV.
typedef enum {
	PD_UI_NO_DEVICE = 0,
	PD_UI_DATA_DISK,
	PD_UI_UNKNOWN,
	PD_UI_CONNECTING,
} pd_ui_state_t;

void pd_ui_draw_idle(pd_ui_state_t state);

// A speaker badge that appears when sound is switched on or off and dissolves
// away over about a second and a half. Draw it last, over whatever is already
// on screen: during streaming that is the live picture, so it has to be redrawn
// every frame rather than left sitting in the framebuffer.
void pd_ui_flash_sound(bool on);
void pd_ui_flash_label(const char *s);
void pd_ui_draw_overlay(void);

#endif // PD_UI_H
