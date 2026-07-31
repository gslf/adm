#ifndef UTF8_H
#define UTF8_H

// Text is stored as UTF-8 bytes and every offset in the editor is a byte
// offset. These helpers translate between the three units that matter:
// bytes, grapheme clusters (what the user calls a character) and terminal
// columns (how much room a cluster takes on screen).

// Decode the sequence at s and store the code point in cp. Returns the bytes
// consumed, always at least 1: an invalid byte decodes to itself, so a broken
// file can still be opened, edited and saved without losing anything.
int utf8_decode(const char *s, int *cp);

// Encode cp into out, which must hold 4 bytes. Returns the bytes written, or
// 0 if cp is not a code point that can be encoded (negative, a surrogate half
// or past U+10FFFF), in which case out is left untouched.
int utf8_encode(int cp, char *out);

// Terminal columns taken by one code point: 0, 1 or 2.
int utf8_width(int cp);

// Byte offset of the cluster boundary after / before byte offset i.
int grapheme_next(const char *s, int i);
int grapheme_prev(const char *s, int i);

// Terminal columns taken by the cluster starting at byte offset i.
int grapheme_width(const char *s, int i);

// Terminal columns taken by s from byte 0 up to (not including) byte upto.
int utf8_cols(const char *s, int upto);

// Byte offset of the cluster covering screen column col, or the end of the
// line if col falls past it. The inverse of utf8_cols.
int utf8_byte_at_col(const char *s, int col);

#endif
