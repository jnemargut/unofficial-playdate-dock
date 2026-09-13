#ifndef PD_FONT_H
#define PD_FONT_H

#include <stdint.h>

// 8x8 bitmap font covering ASCII 32..95 (space through underscore), which is
// space, punctuation, digits and uppercase. Lowercase is folded to uppercase.
// Each glyph is 8 bytes, one per row, MSB = leftmost pixel.
const uint8_t *pd_font_glyph(char c);

#endif // PD_FONT_H
