#include "utf8.h"

typedef struct {
  int lo, hi;
} range;

// Code points that take no room of their own: combining marks, joiners,
// direction marks and variation selectors. Covers the scripts a text editor
// meets in practice; full Unicode coverage would need a generated table.
static const range zero[] = {
  {0x00300, 0x0036F}, // combining diacritical marks
  {0x00483, 0x00489}, // Cyrillic
  {0x00591, 0x005BD}, // Hebrew points
  {0x005BF, 0x005BF},
  {0x005C1, 0x005C2},
  {0x005C4, 0x005C5},
  {0x005C7, 0x005C7},
  {0x00610, 0x0061A}, // Arabic
  {0x0064B, 0x0065F},
  {0x00670, 0x00670},
  {0x006D6, 0x006DC},
  {0x006DF, 0x006E4},
  {0x006E7, 0x006E8},
  {0x006EA, 0x006ED},
  {0x00711, 0x00711}, // Syriac
  {0x00730, 0x0074A},
  {0x007A6, 0x007B0}, // Thaana
  {0x007EB, 0x007F3}, // NKo
  {0x00900, 0x00902}, // Devanagari
  {0x0093C, 0x0093C},
  {0x00941, 0x00948},
  {0x0094D, 0x0094D},
  {0x00951, 0x00957},
  {0x00E31, 0x00E31}, // Thai
  {0x00E34, 0x00E3A},
  {0x00E47, 0x00E4E},
  {0x0135D, 0x0135F}, // Ethiopic
  {0x01AB0, 0x01AFF}, // combining diacritical marks extended
  {0x01DC0, 0x01DFF}, // combining diacritical marks supplement
  {0x0200B, 0x0200F}, // zero width space, joiners, direction marks
  {0x0202A, 0x0202E}, // bidi embedding
  {0x02060, 0x02064}, // word joiner, invisible operators
  {0x020D0, 0x020F0}, // combining marks for symbols
  {0x0FE00, 0x0FE0F}, // variation selectors
  {0x0FE20, 0x0FE2F}, // combining half marks
  {0x1D167, 0x1D169}, // musical notation
  {0x1D17B, 0x1D182},
  {0x1D185, 0x1D18B},
  {0x1D1AA, 0x1D1AD},
  {0xE0100, 0xE01EF}, // variation selectors supplement
};

// Code points drawn two columns wide: CJK, Hangul, fullwidth forms, emoji.
static const range wide[] = {
  {0x01100, 0x0115F}, // Hangul jamo, initial consonants
  {0x02329, 0x0232A},
  {0x02E80, 0x0303E}, // CJK radicals, Kangxi, CJK symbols
  {0x03041, 0x033FF}, // kana, bopomofo, CJK compatibility
  {0x03400, 0x04DBF}, // CJK extension A
  {0x04E00, 0x09FFF}, // CJK unified ideographs
  {0x0A000, 0x0A4CF}, // Yi
  {0x0A960, 0x0A97F}, // Hangul jamo extended A
  {0x0AC00, 0x0D7A3}, // Hangul syllables
  {0x0F900, 0x0FAFF}, // CJK compatibility ideographs
  {0x0FE10, 0x0FE19}, // vertical forms
  {0x0FE30, 0x0FE6F}, // CJK compatibility forms
  {0x0FF00, 0x0FF60}, // fullwidth forms
  {0x0FFE0, 0x0FFE6},
  {0x17000, 0x18AFF}, // Tangut
  {0x1F004, 0x1F004}, // mahjong tile red dragon
  {0x1F0CF, 0x1F0CF}, // playing card black joker
  {0x1F18E, 0x1F19A},
  {0x1F1E6, 0x1F1FF}, // regional indicators, the halves of a flag
  {0x1F200, 0x1F320}, // enclosed ideographic, weather and landscape emoji
  {0x1F32D, 0x1F64F}, // food, activity, people emoji
  {0x1F680, 0x1F6C5}, // transport emoji
  {0x1F900, 0x1F9FF}, // supplemental symbols and pictographs
  {0x1FA70, 0x1FAFF}, // extended A pictographs
  {0x20000, 0x2FFFD}, // CJK extension B and beyond
  {0x30000, 0x3FFFD},
};

static int in_ranges(int cp, const range *r, int n) {
  int lo = 0, hi = n - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (cp < r[mid].lo)
      hi = mid - 1;
    else if (cp > r[mid].hi)
      lo = mid + 1;
    else
      return 1;
  }
  return 0;
}

int utf8_decode(const char *s, int *cp) {
  unsigned char c = (unsigned char)s[0];
  int len, min, v;

  if (c < 0x80) {
    *cp = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    len = 2; min = 0x80;    v = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3; min = 0x800;   v = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4; min = 0x10000; v = c & 0x07;
  } else {
    *cp = c; // stray continuation byte or invalid lead
    return 1;
  }

  for (int i = 1; i < len; i++) {
    if (((unsigned char)s[i] & 0xC0) != 0x80) {
      *cp = c; // truncated sequence, the terminator included
      return 1;
    }
    v = (v << 6) | ((unsigned char)s[i] & 0x3F);
  }

  // Overlong encoding, surrogate half or past the last code point.
  if (v < min || (v >= 0xD800 && v <= 0xDFFF) || v > 0x10FFFF) {
    *cp = c;
    return 1;
  }

  *cp = v;
  return len;
}

int utf8_encode(int cp, char *out) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

int utf8_width(int cp) {
  // Control bytes get one placeholder column rather than disappearing.
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
    return 1;
  if (in_ranges(cp, zero, (int)(sizeof zero / sizeof *zero)))
    return 0;
  if (in_ranges(cp, wide, (int)(sizeof wide / sizeof *wide)))
    return 2;
  return 1;
}

// Code points that always glue onto the cluster before them.
static int is_extend(int cp) {
  return utf8_width(cp) == 0              // marks, joiners, selectors
      || (cp >= 0x1F3FB && cp <= 0x1F3FF) // emoji skin tone modifiers
      || (cp >= 0x1160 && cp <= 0x11FF);  // Hangul conjoining jamo, medial and final
}

// Regional indicators pair up into one flag.
static int is_regional(int cp) {
  return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

int grapheme_next(const char *s, int i) {
  if (!s[i])
    return i;

  int cp, j = i + utf8_decode(s + i, &cp);

  if (is_regional(cp) && s[j]) {
    int cp2, n2 = utf8_decode(s + j, &cp2);
    if (is_regional(cp2))
      j += n2; // exactly two, so adjacent flags stay separate
  }

  while (s[j]) {
    int cp2, n2 = utf8_decode(s + j, &cp2);
    if (!is_extend(cp2))
      break;
    j += n2;

    // A zero width joiner also pulls in whatever follows it.
    if (cp2 == 0x200D && s[j]) {
      int cp3;
      j += utf8_decode(s + j, &cp3);
    }
  }

  return j;
}

int grapheme_prev(const char *s, int i) {
  int j = 0;
  while (j < i) {
    int n = grapheme_next(s, j);
    if (n <= j || n >= i)
      return j;
    j = n;
  }
  return 0;
}

int grapheme_width(const char *s, int i) {
  int end = grapheme_next(s, i);
  int w = 0;

  // The base decides the width; a joined emoji sequence stays as wide as its
  // widest part, which is what terminals actually draw.
  for (int j = i; j < end; ) {
    int cp, n = utf8_decode(s + j, &cp);
    int cw = utf8_width(cp);
    if (cw > w)
      w = cw;
    j += n;
  }

  return w ? w : 1; // a cluster of bare marks still needs somewhere to sit
}

int utf8_cols(const char *s, int upto) {
  int col = 0;
  for (int j = 0; j < upto && s[j]; ) {
    col += grapheme_width(s, j);
    int n = grapheme_next(s, j);
    if (n <= j)
      break;
    j = n;
  }
  return col;
}

int utf8_byte_at_col(const char *s, int col) {
  int c = 0, j = 0;
  while (s[j]) {
    int w = grapheme_width(s, j);
    if (c + w > col)
      return j; // this cluster covers the column, wide ones included
    int n = grapheme_next(s, j);
    if (n <= j)
      break;
    c += w;
    j = n;
  }
  return j;
}
