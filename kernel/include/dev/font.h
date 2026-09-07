/* VoidOS — shared 8×8 bitmap glyph font (public domain)
 *
 * PC BIOS CP437 subset, chars 0x20–0x7E (' '..'~'), 8 bytes per glyph, one
 * byte per scanline, MSB = leftmost pixel.  This is the same font data the
 * kpanic framebuffer renderer always used; it is a public-domain subset of
 * the PC BIOS font, so no license issue attaches to reusing it for the
 * kernel terminal.
 *
 * Lux provenance note: lux's tty.c bundles the *GPLv2* "epto" 8x16 ISO font
 * (src/tty/font.c).  We deliberately do not copy that data or its col-
 * umn-glyph rendering model into Void.  What we take from Lux's tty.c is the
 * *algorithmic shape* of a framebuffer terminal — a glyph is a small bitmap
 * stamped onto the fb, a cursor advances in fixed-width steps, a line of text
 * scrolls the buffer upward at the bottom edge, and a color escape sequence
 * changes the foreground/background — reimplemented against this module's
 * existing 8×8 public-domain font.  See CLAUDE.md Phase 14 (TTY) for the
 * full port classification.
 */
#ifndef VOID_FONT_H
#define VOID_FONT_H 1

/* 95 printable ASCII glyphs, indexed by (c - 0x20).  Bytes are already
 * trimmed to the ' '..'~' range, matching every renderer's guard. */
void font8x8_get(uint8_t uc, const uint8_t out[8]);

#endif /* VOID_FONT_H */