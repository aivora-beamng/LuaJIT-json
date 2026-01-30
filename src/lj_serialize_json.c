/*
** Object JSON de/serialization.
** Copyright (C) 2005-2025 BeamNG GmbH. See Copyright Notice in luajit.h
*/

#define lj_serialize_json_c
#define LUA_CORE

#include "lj_arch.h"
#include "lj_obj.h"

#if LJ_HASJSON

#if LJ_HASBUFFER
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_serialize_json.h"
#include "lj_str.h"
#include "lj_strfmt.h"
#include "lj_strscan.h"
#include "lj_tab.h"
#include "lj_udata.h"

#include <ctype.h>
#include <limits.h>
#include <string.h> // for memchr

// uncomment when debugging this module (to be able to step into inlined functions)
// #define LJ_JSON_DEBUGGING

// optimization tweaks
#define LJ_JSON_BRANCH_HINTS 1
#define LJ_JSON_FORCE_INLINE 1
#define LJ_JSON_USE_JUMPTABLE 1
#define LJ_JSON_SCRATCH_INITIAL_CAPACITY 8192
#define LJ_JSON_ESCAPED_STR_SIZE 128

// to enable SSE4 intrinsics, uncomment the appropriate macro (and build with -msse4)
#define LJ_JSON_USE_INTRINSICS 1
#define LJ_JSON_USE_SSE2
#define LJ_JSON_USE_ARM_NEON
// #define LJ_JSON_USE_SSE4

#ifdef LJ_JSON_DEBUGGING
#undef LJ_JSON_FORCE_INLINE
#define LJ_JSON_FORCE_INLINE 0
#endif

#if LJ_JSON_BRANCH_HINTS
#define LJ_JSON_LIKELY LJ_LIKELY
#define LJ_JSON_UNLIKELY LJ_UNLIKELY
#else
#define LJ_JSON_LIKELY
#define LJ_JSON_UNLIKELY
#endif

#if LJ_JSON_FORCE_INLINE
#define LJ_JSON_INLINE LJ_INLINE
#define LJ_JSON_AINLINE LJ_AINLINE
#else
#define LJ_JSON_INLINE
#define LJ_JSON_AINLINE
#endif

#if LJ_JSON_USE_INTRINSICS
#if LJ_TARGET_ARM64 && defined(LJ_JSON_USE_ARM_NEON)
#if LJ_TARGET_WINDOWS
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#elif defined(LJ_JSON_USE_SSE4)
#include <nmmintrin.h>
#elif defined(LJ_JSON_USE_SSE2)
#include <emmintrin.h>
#else
#error "No known intrinsics for current target"
#endif
#endif // LJ_JSON_USE_INTRINSICS

/* -- Internal serializer ------------------------------------------------- */

// JSON encoding start

static LJ_JSON_AINLINE char *lj_json_serialize_more(char *w, SBufExt *sbx, MSize sz) {
  if (LJ_JSON_UNLIKELY(sz > (MSize)(sbx->e - w))) {
    w = lj_buf_more2((SBuf *)sbx, sz);
  }
  return w;
}

char *lj_strfmt_wfnum(SBuf *sb, SFormat sf, lua_Number n, char *p);

static LJ_JSON_AINLINE char *lj_json_put_number(char *w, SBufExt *sbx, lua_Number n) {
  TValue t;
  t.n = n;
  if (LJ_JSON_UNLIKELY((t.u32.hi << 1) >= 0xffe00000)) {
    if (((t.u32.hi & 0x000fffff) | t.u32.lo) != 0) {
      w = lj_json_serialize_more(w, sbx, 3);
      memcpy(w, "NaN", 3); w += 3;
    } else {
      if ((t.u32.hi & 0x80000000)) {
        w = lj_json_serialize_more(w, sbx, 1);
        *w++ = '-';
      }
      w = lj_json_serialize_more(w, sbx, 8);
      memcpy(w, "Infinity", 8); w += 8;
    }
  } else {
    char tmp[STRFMT_MAXBUF_NUM];
    MSize len = (MSize)(lj_strfmt_wfnum(NULL, STRFMT_G14, n, tmp) - tmp);
    w = lj_json_serialize_more(w, sbx, len);
    memcpy(w, tmp, len);
    w += len;
  }
  return w;
}

static LJ_JSON_AINLINE char lj_json_unescape(char c) {
  switch (c) {
  case '\t':
    return 't';
  case '\n':
    return 'n';
  case '\f':
    return 'f';
  case '\r':
    return 'r';
  case '\b':
    return 'b';
  case '\\':
    return '\\';
  case '"':
    return '"';
  default:
    return 0;
  }
}

static LJ_JSON_AINLINE char *lj_json_put_string(char *w, SBufExt *sbx, const GCstr *str) {
  MSize len = str->len;
  w = lj_json_serialize_more(w, sbx, len);
  const char *data = strdata(str);
  for (MSize i = 0; i < str->len; i++) {
    char unescaped = lj_json_unescape(data[i]);
    if (LJ_JSON_UNLIKELY(unescaped)) {
      len++;
      w = lj_json_serialize_more(w, sbx, len);
      *w++ = '\\';
      *w++ = unescaped;
    } else {
      *w++ = data[i];
    }
  }
  return w;
}

static LJ_JSON_AINLINE char *lj_json_put_bool(char *w, SBufExt *sbx, uint32_t itype) {
  // possible values: null true false
  w = lj_json_serialize_more(w, sbx, 5);
  switch (itype) {
  case LJ_TNIL:
    memcpy(w, "null", 4); w += 4;
    break;
  case LJ_TTRUE:
    memcpy(w, "true", 4); w += 4;
    break;
  case LJ_TFALSE:
    memcpy(w, "false", 5); w += 5;
    break;
  }
  return w;
}

static char *lj_json_put_table_key(char *w, SBufExt *sbx, cTValue *o) {
  if (LJ_JSON_LIKELY(tvisstr(o))) {
    w = lj_json_put_string(w, sbx, strV(o));
  } else if (tvisint(o)) {
    uint32_t x = (uint32_t)intV(o);
    w = lj_json_serialize_more(w, sbx, STRFMT_MAXBUF_INT);
    w = lj_strfmt_wint(w, x);
  } else if (tvisnum(o)) {
    w = lj_json_put_number(w, sbx, numV(o));
  } else if (tvispri(o)) {
    w = lj_json_put_bool(w, sbx, itype(o));
  }
  return w;
}

static char *lj_json_serialize_put(char *w, SBufExt *sbx, cTValue *o);

static LJ_JSON_AINLINE char *lj_json_put_table(char *w, SBufExt *sbx, const GCtab *t) {
  uint32_t narray = 0, nhash = 0, one = 2;
  if (LJ_JSON_UNLIKELY(sbx->depth <= 0)) lj_err_caller(sbufL(sbx), LJ_ERR_BUFFER_DEPTH);
  sbx->depth--;
  if (t->asize > 0) {  /* Determine max. length of array part. */
    ptrdiff_t i;
    TValue *array = tvref(t->array);
    for (i = (ptrdiff_t)t->asize-1; i >= 0; i--)
      if (!tvisnil(&array[i]))
        break;
    narray = (uint32_t)(i+1);
    if (narray && tvisnil(&array[0])) one = 4;
  }
  if (t->hmask > 0) {  /* Count number of used hash slots. */
    uint32_t i, hmask = t->hmask;
    Node *node = noderef(t->node);
    for (i = 0; i <= hmask; i++)
      nhash += !tvisnil(&node[i].val);
  }
  if (nhash) { /* Dictionary. */
    char prefix = '{';
    if (narray) { /* Write array part with int->string key conversion. */
      uint32_t idx = 1;
      cTValue *oa = tvref(t->array) + (one >> 2);
      cTValue *oe = tvref(t->array) + narray;

      while (oa < oe) {
        w = lj_json_serialize_more(w, sbx, 2);
        *w++ = prefix;
        *w++ = '"';
        w = lj_json_serialize_more(w, sbx, STRFMT_MAXBUF_INT);
        w = lj_strfmt_wint(w, idx);
        w = lj_json_serialize_more(w, sbx, 2);
        *w++ = '"';
        *w++ = ':';
        prefix = ',';
        w = lj_json_serialize_put(w, sbx, oa++);
        ++idx;
      }
    }

    Node *node = noderef(t->node) + t->hmask;
    for (;; node--) /* Write hash part. */
      if (!tvisnil(&node->val)) {
        w = lj_json_serialize_more(w, sbx, 2);
        *w++ = prefix;
        *w++ = '"';
        w = lj_json_put_table_key(w, sbx, &node->key);
        w = lj_json_serialize_more(w, sbx, 2);
        *w++ = '"';
        *w++ = ':';
        prefix = ',';
        w = lj_json_serialize_put(w, sbx, &node->val);
        if (--nhash == 0) break;
      }
    w = lj_json_serialize_more(w, sbx, 1);
    *w++ = '}';
  } else if (narray) { /* Write array entries. */
    cTValue *oa = tvref(t->array) + (one >> 2);
    cTValue *oe = tvref(t->array) + narray;
    w = lj_json_serialize_more(w, sbx, 1);
    *w++ = '[';
    if (oa != oe) {
      w = lj_json_serialize_put(w, sbx, oa++);
    }
    while (oa < oe) {
      w = lj_json_serialize_more(w, sbx, 1);
      *w++ = ',';
      w = lj_json_serialize_put(w, sbx, oa++);
    }
    w = lj_json_serialize_more(w, sbx, 1);
    *w++ = ']';
  } else { /* Empty dictionary. */
    w = lj_json_serialize_more(w, sbx, 2);
    *w++ = '{';
    *w++ = '}';
  }
  sbx->depth++;
  return w;
}

static char *lj_json_serialize_put(char *w, SBufExt *sbx, cTValue *o) {
  if (LJ_JSON_LIKELY(tvisstr(o))) {
    w = lj_json_serialize_more(w, sbx, 1);
    *w++ = '"';
    w = lj_json_put_string(w, sbx, strV(o));
    w = lj_json_serialize_more(w, sbx, 1);
    *w++ = '"';
  } else if (tvisint(o)) {
    uint32_t x = (uint32_t)intV(o);
    w = lj_json_serialize_more(w, sbx, STRFMT_MAXBUF_INT);
    w = lj_strfmt_wint(w, x);
  } else if (tvisnum(o)) {
    w = lj_json_put_number(w, sbx, numV(o));
  } else if (tvispri(o)) {
    w = lj_json_put_bool(w, sbx, itype(o));
  } else if (tvistab(o)) {
    w = lj_json_put_table(w, sbx, tabV(o));
  } else {
    // we got unserializable data type :/ proceed anyways for compatibility
    // lj_err_callerv(sbufL(sbx), LJ_ERR_BUFFER_BADENC, lj_typename(o));
    w = lj_json_serialize_more(w, sbx, 4);
    memcpy(w, "null", 4);
    w += 4;
  }
  return w;
}

// JSON encoding END

// JSON decoding START

static TValue *lj_json_scratch = NULL;
static uint32_t lj_json_scratch_count = 0;
static uint32_t lj_json_scratch_capacity = 0;

#define LJ_JSON_MAX_SCRATCH INT_MAX-2

static void lj_json_scratch_init(lua_State *L) {
  lj_json_scratch = lj_mem_newvec(L, LJ_JSON_SCRATCH_INITIAL_CAPACITY, TValue);
  lj_json_scratch_capacity = LJ_JSON_SCRATCH_INITIAL_CAPACITY;
}

static void lj_json_scratch_free(lua_State* L) {
  if (!lj_json_scratch) {
    return;
  }
  lj_mem_freevec(G(L), lj_json_scratch, lj_json_scratch_capacity, TValue);
  lj_json_scratch = NULL;
  lj_json_scratch_count = 0;
  lj_json_scratch_capacity = 0;
}

static void lj_json_scratch_reset() {
  lj_json_scratch_count = 0;
}

#define scratchV(i) (lj_json_scratch[i])

// allocate space for n TValues and return index of the first one on the scratch buffer
static LJ_JSON_AINLINE uint32_t lj_json_scratch_pushn(lua_State *L, uint32_t n) {
  if (LJ_UNLIKELY(lj_json_scratch_capacity == 0)) {
    lj_json_scratch_init(L);
  } else if (LJ_UNLIKELY(lj_json_scratch_count + n >= lj_json_scratch_capacity)) {
    if (LJ_UNLIKELY(lj_json_scratch_capacity == LJ_JSON_MAX_SCRATCH)) {
      lj_err_caller(L, LJ_ERR_ERRMEM);
    }
    lj_mem_growvec(L, lj_json_scratch, lj_json_scratch_capacity, LJ_JSON_MAX_SCRATCH, TValue);
  }
  uint32_t scr = lj_json_scratch_count;
  lj_json_scratch_count += n;
  return scr;
}

// "remove" n TValues on the stack and return index of the first one
static LJ_JSON_AINLINE uint32_t lj_json_scratch_popn(lua_State *L, uint32_t n) {
  if (LJ_UNLIKELY(n > lj_json_scratch_count)) {
    lj_err_caller(L, LJ_ERR_ERRMEM);
  }
  lj_json_scratch_count -= n;
  return lj_json_scratch_count;
}

static const char *lj_err_json_geterror(SBufExt *sbx, char *r, int *line, int *col) {
  lj_json_scratch_free(sbufL(sbx));
  static char tmp_err_buffer[128] = {0};
  char *curnewline = sbx->r;
  *line = 1;
  *col = 1;
  char *next = NULL;
  while (curnewline < r) {
    next = memchr(curnewline, '\n', sbx->w - curnewline);
    if (!next || next >= r) {
      break;
    }
    curnewline = next + 1;
    (*line)++;
  }

  if (!next) {
    next = sbx->w;
  }
  *col = (int)(r - curnewline + 1);
  ptrdiff_t msglen = next - r;
  if (msglen >= 128) {
    msglen = 127;
  } else if (msglen < 0) {
    msglen = 0;
  }
  memcpy(tmp_err_buffer, r, msglen);
  tmp_err_buffer[msglen] = 0;
  return tmp_err_buffer;
}

#define lj_err_json(sbx, em) do {                                          \
  int line, col;                                                           \
  const char *msg = lj_err_json_geterror(sbx, r, &line, &col);             \
  lj_err_callerv((sbufL(sbx)), em, line, col, msg);                        \
} while (0);

#define lj_err_jsonv(sbx, em, ...) do {                                    \
  int line, col;                                                           \
  const char *msg = lj_err_json_geterror(sbx, r, &line, &col);             \
  lj_err_callerv((sbufL(sbx)), em, line, col, msg, __VA_ARGS__);           \
} while (0);

#if LJ_JSON_USE_JUMPTABLE
#define JSON_KEY 0x02 // [a-zA-Z0-9_]
#define JSON_WS 0x01

static const unsigned char lookup[256] = {
  JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS,
  JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS,
  JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS,
  JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS, JSON_WS,

  // ' ', ',', '0'-'9'
  JSON_WS, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, JSON_WS, 0, 0, 0,
  JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY,
  JSON_KEY, JSON_KEY, 0, 0, 0, 0, 0, 0,
  // 'A'-'Z', '_'
  0, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY,
  JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY,
  JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY,
  JSON_KEY, JSON_KEY, JSON_KEY, 0, 0, 0, 0, JSON_KEY,

  // 'a'-'z'
  0, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY,
  JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY,
  JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY, JSON_KEY,
  JSON_KEY, JSON_KEY, JSON_KEY, 0, 0, 0, 0, 0,
};

#define isjsonws(x) (lookup[(unsigned char)x] == JSON_WS)
#define isjsonkey(x) (lookup[(unsigned char)x] == JSON_KEY)
#else
#define isjsonws(x) ((x <= ' ' || x == ','))
#define isjsonkey(x) ((isalnum(x) || x == '_'))
#endif

static LJ_JSON_AINLINE char *lj_json_skip_white_space_inner(char *r, SBufExt *sbx) {
  while (LJ_JSON_LIKELY(r < sbx->w) && (isjsonws(*r))) { // matches space tab newline or comma
    r++;
  }
  return r;
}

#if LJ_JSON_USE_INTRINSICS
#if LJ_TARGET_ARM64 && defined(LJ_JSON_USE_ARM_NEON)
static LJ_JSON_AINLINE uint32_t clzll(uint64_t x) {
#if defined(_MSC_VER)
    unsigned long r = 0;
    _BitScanReverse64(&r, x);
    return 63 - r;
#else
    return (uint32_t)__builtin_clzll(x);
#endif
}

static LJ_JSON_AINLINE char *lj_json_skip_until_single_char_simd(char c, char *p, SBufExt *sbx) {
  if (LJ_JSON_UNLIKELY(p == sbx->w)) {
    return NULL;
  }
  const uint8x16_t w0 = vdupq_n_u8((uint8_t)c);
  for (; p <= sbx->w - 16; p += 16) {
    const uint8x16_t s = vld1q_u8((const uint8_t *)(p));
    uint8x16_t x = vceqq_u8(s, w0);
    x = vrev64q_u8(x);                     // Rev in 64
    uint64_t low = vgetq_lane_u64(vreinterpretq_u64_u8(x), 0);   // extract
    uint64_t high = vgetq_lane_u64(vreinterpretq_u64_u8(x), 1);  // extract
    if (low == 0) {
      if (high != 0) {
        uint32_t lz = clzll(high);
        return p + 8 + (lz >> 3);
      }
    } else {
      uint32_t lz = clzll(low);
      return p + (lz >> 3);
    }
  }
  return NULL;
}

static LJ_JSON_AINLINE char *lj_json_skip_to_string_end_simd(char *p, SBufExt *sbx) {
  // Fast return for empty string
  if (LJ_JSON_LIKELY(p != sbx->w) && (*p != '"' && *p != '\\')) {
    ++p;
  } else {
    return p;
  }
  const uint8x16_t w0 = vmovq_n_u8('"');
  const uint8x16_t w1 = vmovq_n_u8('\\');
  for (; p <= sbx->w - 16; p += 16) {
    const uint8x16_t s = vld1q_u8((const uint8_t *)(p));
    uint8x16_t x = vceqq_u8(s, w0);
    x = vorrq_u8(x, vceqq_u8(s, w1));
    //x = vmvnq_u8(x);                       // Negate
    x = vrev64q_u8(x);                     // Rev in 64
    uint64_t low = vgetq_lane_u64(vreinterpretq_u64_u8(x), 0);   // extract
    uint64_t high = vgetq_lane_u64(vreinterpretq_u64_u8(x), 1);  // extract
    if (low == 0) {
      if (high != 0) {
        uint32_t lz = clzll(high);
        return p + 8 + (lz >> 3);
      }
    } else {
      uint32_t lz = clzll(low);
      return p + (lz >> 3);
    }
  }
  return NULL;
}

static LJ_JSON_AINLINE char *lj_json_skip_white_space_simd(char *p, SBufExt *sbx) {
  // Fast return for single non-whitespace
  if (LJ_JSON_LIKELY(p != sbx->w) && (isjsonws(*p))) {
    ++p;
  } else {
    return p;
  }
  const uint8x16_t w0 = vmovq_n_u8(' ');
  const uint8x16_t w1 = vmovq_n_u8('\n');
  const uint8x16_t w2 = vmovq_n_u8('\r');
  const uint8x16_t w3 = vmovq_n_u8('\t');
  const uint8x16_t w4 = vmovq_n_u8(',');
  for (; p <= sbx->w - 16; p += 16) {
    const uint8x16_t s = vld1q_u8((const uint8_t *)(p));
    uint8x16_t x = vceqq_u8(s, w0);
    x = vorrq_u8(x, vceqq_u8(s, w1));
    x = vorrq_u8(x, vceqq_u8(s, w2));
    x = vorrq_u8(x, vceqq_u8(s, w3));
    x = vorrq_u8(x, vceqq_u8(s, w4));
    x = vmvnq_u8(x);                       // Negate
    x = vrev64q_u8(x);                     // Rev in 64
    uint64_t low = vgetq_lane_u64(vreinterpretq_u64_u8(x), 0);   // extract
    uint64_t high = vgetq_lane_u64(vreinterpretq_u64_u8(x), 1);  // extract
    if (low == 0) {
      if (high != 0) {
        uint32_t lz = clzll(high);
        return p + 8 + (lz >> 3);
      }
    } else {
      uint32_t lz = clzll(low);
      return p + (lz >> 3);
    }
  }
  return lj_json_skip_white_space_inner(p, sbx);
}
#elif defined(LJ_JSON_USE_SSE4)
static LJ_JSON_AINLINE char *lj_json_skip_until_single_char_simd(char c, char *p, SBufExt *sbx) {
  if (LJ_JSON_UNLIKELY(p == sbx->w)) {
    return NULL;
  }
  const __m128i w = _mm_set1_epi8(c);
  for (; p <= sbx->w - 16; p += 16) {
    const __m128i s = _mm_loadu_si128((const __m128i *)(p));
    const int r = _mm_cmpistri(w, s, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY | _SIDD_LEAST_SIGNIFICANT);
    if (r != 16) {   // some of characters is c
      return p + r;
    }
  }
  return NULL;
}

static LJ_JSON_AINLINE char *lj_json_skip_to_string_end_simd(char *p, SBufExt *sbx) {
  // Fast return for empty string
  if (LJ_JSON_LIKELY(p != sbx->w) && (*p != '"' && *p != '\\')) {
    ++p;
  } else {
    return p;
  }
  // The rest of string using SIMD
  static const char strend[16] = "\"\\";
  const __m128i w = _mm_loadu_si128((const __m128i *)(&strend[0]));
  for (; p <= sbx->w - 16; p += 16) {
    const __m128i s = _mm_loadu_si128((const __m128i *)(p));
    const int r = _mm_cmpistri(w, s, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY | _SIDD_LEAST_SIGNIFICANT);
    if (r != 16) {   // some of characters is strend
      return p + r;
    }
  }
  return NULL;
}

static LJ_JSON_AINLINE char *lj_json_skip_white_space_simd(char *p, SBufExt *sbx) {
  // Fast return for single non-whitespace
  if (LJ_JSON_LIKELY(p != sbx->w) && (isjsonws(*p))) {
    ++p;
  } else {
    return p;
  }
  // The rest of string using SIMD
  static const char whitespace[16] = " \n\r\t,";
  const __m128i w = _mm_loadu_si128((const __m128i *)(&whitespace[0]));
  for (; p <= sbx->w - 16; p += 16) {
    const __m128i s = _mm_loadu_si128((const __m128i *)(p));
    const int r = _mm_cmpistri(w, s, _SIDD_UBYTE_OPS | _SIDD_CMP_EQUAL_ANY | _SIDD_LEAST_SIGNIFICANT | _SIDD_NEGATIVE_POLARITY);
    if (r != 16) {   // some of characters is non-whitespace
      return p + r;
    }
  }
  return lj_json_skip_white_space_inner(p, sbx);
}
#elif defined(LJ_JSON_USE_SSE2)
#define C16(c) { c, c, c, c, c, c, c, c, c, c, c, c, c, c, c, c }

static LJ_JSON_AINLINE char *lj_json_skip_until_single_char_simd(char c, char *p, SBufExt *sbx) {
  if (LJ_JSON_UNLIKELY(p == sbx->w)) {
    return NULL;
  }
  static char carr[16];
  memset(carr, c, 16);
  const __m128i w0 = _mm_loadu_si128((const __m128i *)(carr));
  for (; p <= sbx->w - 16; p += 16) {
    const __m128i s = _mm_loadu_si128((const __m128i *)(p));
    __m128i x = _mm_cmpeq_epi8(s, w0);
    unsigned short r = (unsigned short)(_mm_movemask_epi8(x));
    if (r != 0) {   // some of characters may be strend
#ifdef _MSC_VER         // Find the index of first strend
      unsigned long offset;
      _BitScanForward(&offset, r);
      return p + offset;
#else
      return p + __builtin_ffs(r) - 1;
#endif
    }
  }
  return NULL;
}

static LJ_JSON_AINLINE char *lj_json_skip_to_string_end_simd(char *p, SBufExt *sbx) {
  // Fast return for empty string
  if (LJ_JSON_LIKELY(p != sbx->w) && (*p != '"' && *p != '\\')) {
    ++p;
  } else {
    return p;
  }
  // The rest of string
  static const char strend[2][16] = { C16('"'), C16('\\') };
  const __m128i w0 = _mm_loadu_si128((const __m128i *)(&strend[0][0]));
  const __m128i w1 = _mm_loadu_si128((const __m128i *)(&strend[1][0]));
  for (; p <= sbx->w - 16; p += 16) {
    const __m128i s = _mm_loadu_si128((const __m128i *)(p));
    __m128i x = _mm_cmpeq_epi8(s, w0);
    x = _mm_or_si128(x, _mm_cmpeq_epi8(s, w1));
    unsigned short r = (unsigned short)(_mm_movemask_epi8(x));
    if (r != 0) {   // some of characters may be strend
#ifdef _MSC_VER         // Find the index of first strend
      unsigned long offset;
      _BitScanForward(&offset, r);
      return p + offset;
#else
      return p + __builtin_ffs(r) - 1;
#endif
    }
  }
  return NULL;
}

static LJ_JSON_AINLINE char *lj_json_skip_white_space_simd(char *p, SBufExt *sbx) {
  // Fast return for single non-whitespace
  if (LJ_JSON_LIKELY(p != sbx->w) && (isjsonws(*p))) {
    ++p;
  } else {
    return p;
  }
  // The rest of string
  static const char whitespaces[6][16] = { C16(' '), C16('\n'), C16('\r'), C16('\t'), C16(',') };
  const __m128i w0 = _mm_loadu_si128((const __m128i *)(&whitespaces[0][0]));
  const __m128i w1 = _mm_loadu_si128((const __m128i *)(&whitespaces[1][0]));
  const __m128i w2 = _mm_loadu_si128((const __m128i *)(&whitespaces[2][0]));
  const __m128i w3 = _mm_loadu_si128((const __m128i *)(&whitespaces[3][0]));
  const __m128i w4 = _mm_loadu_si128((const __m128i *)(&whitespaces[4][0]));
  for (; p <= sbx->w - 16; p += 16) {
    const __m128i s = _mm_loadu_si128((const __m128i *)(p));
    __m128i x = _mm_cmpeq_epi8(s, w0);
    x = _mm_or_si128(x, _mm_cmpeq_epi8(s, w1));
    x = _mm_or_si128(x, _mm_cmpeq_epi8(s, w2));
    x = _mm_or_si128(x, _mm_cmpeq_epi8(s, w3));
    x = _mm_or_si128(x, _mm_cmpeq_epi8(s, w4));
    unsigned short r = (unsigned short)(~_mm_movemask_epi8(x));
    if (r != 0) {   // some of characters may be non-whitespace
#ifdef _MSC_VER         // Find the index of first non-whitespace
      unsigned long offset;
      _BitScanForward(&offset, r);
      return p + offset;
#else
      return p + __builtin_ffs(r) - 1;
#endif
    }
  }
  return lj_json_skip_white_space_inner(p, sbx);
}
#undef C16
#endif // LJ_JSON_USE_SSE2
#endif // LJ_JSON_USE_INTRINSICS

static LJ_JSON_AINLINE char *lj_json_skip_comment_space(char *r, SBufExt *sbx) {
  if (LJ_JSON_UNLIKELY(r >= sbx->w)) {
    return r;
  }
  switch (*r++) {
  case '/': { // / -- single line comment "//"
#if LJ_JSON_USE_INTRINSICS
    char *tmp = lj_json_skip_until_single_char_simd('\n', r, sbx);
    if (tmp) {
      r = tmp;
      r++;
    } else
#endif // LJ_JSON_USE_INTRINSICS
    {
      while (LJ_JSON_LIKELY(r < sbx->w) && *r != '\n') {
        r++;
      }
      if (LJ_JSON_LIKELY(r < sbx->w)) {
        r++;
      }
    }
    break;
  }
  case '*': { // * -- block comment "/*  xxxxxxx */"
    if (LJ_JSON_UNLIKELY(r >= sbx->w)) {
      lj_err_json(sbx, LJ_ERR_BADJSON_INVALIDCOMM);
    }
    while (LJ_JSON_LIKELY(r < sbx->w)) {
#if LJ_JSON_USE_INTRINSICS
      char *tmp = lj_json_skip_until_single_char_simd('*', r, sbx);
      if (tmp) {
        r = tmp;
      }
#endif // LJ_JSON_USE_INTRINSICS
      if (*r != '*') {
        r++;
        continue;
      }
      r++;
      if (LJ_JSON_UNLIKELY(r >= sbx->w)) {
        lj_err_json(sbx, LJ_ERR_BADJSON_INVALIDCOMM);
      }
      if (*r == '/') {
        r++;
        return r;
      }
      if (r[-2] == '/') {
        r -= 2;
        lj_err_json(sbx, LJ_ERR_BADJSON_NESTEDCOMM);
      }
    }
    lj_err_json(sbx, LJ_ERR_BADJSON_INVALIDCOMM);
  }
  default:
    lj_err_json(sbx, LJ_ERR_BADJSON_INVALIDCOMM);
  }
  return r;
}

static LJ_JSON_AINLINE char *lj_json_skip_white_space(char *r, SBufExt *sbx) {
#if LJ_JSON_USE_INTRINSICS
  r = lj_json_skip_white_space_simd(r, sbx);
#else
  r = lj_json_skip_white_space_inner(r, sbx);
#endif
  while (LJ_JSON_LIKELY(r < sbx->w) && *r == '/') {
    r++;
    r = lj_json_skip_comment_space(r, sbx); // / -- read comment
#if LJ_JSON_USE_INTRINSICS
  r = lj_json_skip_white_space_simd(r, sbx);
#else
  r = lj_json_skip_white_space_inner(r, sbx);
#endif
  }
  return r;
}

static char *lj_json_read_number(char *r, SBufExt *sbx, TValue *o) {
  char *rbegin = r;

  TValue tmp;
  tmp.n = 0.0;
  while (LJ_JSON_LIKELY(r < sbx->w)) {
    const unsigned char digit = (unsigned char)(*r - '0');
    if (digit > 9) {
      break;
    }
    tmp.n = 10 * tmp.n + digit;
    r++;
  }
  char cont = 1;
  while (LJ_JSON_LIKELY(r < sbx->w) & cont) {
    switch (*r) {
    case '.': {
      lua_Number f = 0, scale = 0.1;
      r++;
      while (LJ_JSON_LIKELY(r < sbx->w)) {
        const unsigned char digit = (unsigned char)(*r - '0');
        if (digit > 9) {
          break;
        }
        f += digit * scale;
        scale *= 0.1;
        r++;
      }
      tmp.n += f;
      break;
    }
    case '#': {
      const char val[7] = "1#INF00";
      if ((rbegin + sizeof(val) <= sbx->w && strncmp(rbegin, val, sizeof(val)) == 0)) {
        setpinfV(o);
        return rbegin + sizeof(val);
      }
      r = rbegin;
      lj_err_json(sbx, LJ_ERR_BADJSON_INVALIDNUM);
      return NULL;
    }
    case 'e':
    case 'E': {
      r++;
      while (LJ_JSON_LIKELY(r < sbx->w) && ((*r >= '-' && *r <= '9') || *r == '+')) {
        r++;
      }
      char back = *r;
      *r = 0;
      StrScanFmt fmt = lj_strscan_scan((const uint8_t *)rbegin, (MSize)(r - rbegin), &tmp, STRSCAN_OPT_TONUM);
      *r = back;
      if (fmt == STRSCAN_ERROR) {
        r = rbegin;
        lj_err_json(sbx, LJ_ERR_BADJSON_INVALIDNUM);
        return NULL;
      }
      break;
    }
    default:
      cont = 0;
      break;
    }
  }
  if (rbegin == r) {
    lj_err_json(sbx, LJ_ERR_BADJSON_INVALIDNUM);
  }
  o->u64 = tmp.u64;
  return r;
}

static LJ_JSON_AINLINE char lj_json_get_escape(char c) {
  switch (c) {
  case 't':
  case '9':
    return '\t';
  case 'n':
    return '\n';
  case 'f':
    return '\f';
  case 'r':
  case '0':
    return '\r';
  case 'b':
    return '\b';
  default:
    return c;
  }
}

static char *lj_json_read_escaped_string(char *r, char *rbegin, char *escape, SBufExt *sbx, GCstr **str) {
  char backer[LJ_JSON_ESCAPED_STR_SIZE] = {0};

  size_t tmpsz = r - rbegin;
  char allocate = tmpsz >= LJ_JSON_ESCAPED_STR_SIZE;
  char *tmp;
  lua_State *L = sbufL(sbx);
  if (allocate) {
    tmp = (char *)lj_mem_new(L, tmpsz);
  } else {
    tmp = backer;
  }

  ptrdiff_t i = escape - rbegin;
  memcpy(tmp, rbegin, i);

  tmp[i++] = lj_json_get_escape(escape[1]);
  r = escape + 2;
  while (LJ_JSON_LIKELY(r < sbx->w)) {
    if (*r == '\\') {
      r++;
      tmp[i++] = lj_json_get_escape(*r);
    } else {
      if (*r == '"') {
        break;
      }
      tmp[i++] = *r;
    }
    r++;
  }

  *str = lj_str_new(L, tmp, i);
  if (allocate) {
    lj_mem_free(G(L), tmp, tmpsz);
  }
  return r + 1;
}

static LJ_JSON_AINLINE char *lj_json_read_string(char *r, SBufExt *sbx, GCstr **str) {
  char *rbegin = r;
  char *escape = NULL;
#if LJ_JSON_USE_INTRINSICS
  char *tmp = lj_json_skip_to_string_end_simd(r, sbx);
  if (LJ_JSON_LIKELY(tmp)) {
    r = tmp;
  }
  if (LJ_JSON_UNLIKELY(r < sbx->w && *r != '"'))
#endif
  {
    while (LJ_JSON_LIKELY(r < sbx->w)) {
      if (*r == '\\') {
        if (!escape) {
          escape = r;
        }
        r++;
        if (LJ_JSON_UNLIKELY(r >= sbx->w)) {
          break;
        }
      } else if (*r == '"') {
        break;
      }
      r++;
    }
  }

  if (LJ_JSON_UNLIKELY(r >= sbx->w)) {
    lj_err_json(sbx, LJ_ERR_BADJSON_MISSINGEND);
  }

  if (LJ_JSON_LIKELY(!escape)) {
    *str = lj_str_new(sbufL(sbx), rbegin, r - rbegin);
    return r + 1;
  }

  return lj_json_read_escaped_string(r, rbegin, escape, sbx, str);
}

static LJ_JSON_AINLINE char *lj_json_read_key(char *r, SBufExt *sbx, uint32_t scr) {
  if (LJ_JSON_LIKELY(r < sbx->w) && *r == '"') {
    GCstr* str;
    r = lj_json_read_string(r + 1, sbx, &str);
    scratchV(scr).u64 = (uintptr_t)str;
  } else {
    char *rbegin = r;
    while (LJ_JSON_LIKELY(r < sbx->w) && isjsonkey(*r)) {
      r++;
    }
    if (r == rbegin) {
      lj_err_json(sbx, LJ_ERR_BADJSON_MISSINGDICTKEY);
      return NULL;
    }

    GCstr* str = lj_str_new(sbufL(sbx), rbegin, r - rbegin);
    scratchV(scr).u64 = (uintptr_t)str;
  }

  r = lj_json_skip_white_space(r, sbx);
  if (LJ_JSON_UNLIKELY(r >= sbx->w || (*r != ':' && *r != '='))) {
    lj_err_jsonv(sbx, LJ_ERR_BADJSON_INVALIDSEP, *r);
    return NULL;
  }

  return r + 1;
}

static LJ_JSON_AINLINE char *lj_json_read_nan(char *r, SBufExt *sbx, TValue *o) {
  if (LJ_JSON_UNLIKELY(r + 3 > sbx->w || r[1] != 'a' || r[2] != 'N')) {
    lj_err_jsonv(sbx, LJ_ERR_BADJSON_INVALIDVAL, "NaN");
  }
  setnanV(o);
  return r + 3;
}

static LJ_JSON_AINLINE char *lj_json_read_infinity(char *r, SBufExt *sbx, char neg, TValue *o) {
  if (LJ_JSON_UNLIKELY(r + 8 > sbx->w || r[1] != 'n' || r[2] != 'f' || r[3] != 'i' || r[4] != 'n' || r[5] != 'i' || r[6] != 't' || r[7] != 'y')) {
    lj_err_jsonv(sbx, LJ_ERR_BADJSON_INVALIDVAL, "Infinity");
  }
  if (neg) {
    setminfV(o);
  } else {
    setpinfV(o);
  }
  return r + 8;
}

static LJ_JSON_AINLINE char *lj_json_read_true(char *r, SBufExt *sbx, TValue *o) {
  if (LJ_JSON_UNLIKELY(r + 4 > sbx->w || r[1] != 'r' || r[2] != 'u' || r[3] != 'e')) {
    lj_err_jsonv(sbx, LJ_ERR_BADJSON_INVALIDVAL, "true");
  }
  setboolV(o, 1);
  return r + 4;
}

static LJ_JSON_AINLINE char *lj_json_read_false(char *r, SBufExt *sbx, TValue *o) {
  if (LJ_JSON_UNLIKELY(r + 5 > sbx->w || r[1] != 'a' || r[2] != 'l' || r[3] != 's' || r[4] != 'e')) {
    lj_err_jsonv(sbx, LJ_ERR_BADJSON_INVALIDVAL, "false");
  }
  setboolV(o, 0);
  return r + 5;
}

static LJ_JSON_AINLINE char *lj_json_read_null(char *r, SBufExt *sbx, TValue *o) {
  if (LJ_JSON_UNLIKELY(r + 4 > sbx->w || r[1] != 'u' || r[2] != 'l' || r[3] != 'l')) {
    lj_err_jsonv(sbx, LJ_ERR_BADJSON_INVALIDVAL, "null");
  }
  setnilV(o);
  return r + 4;
}

static LJ_JSON_AINLINE char *lj_json_read_minus(char *r, SBufExt *sbx, TValue *o) {
  if (LJ_JSON_UNLIKELY(r >= sbx->w)) {
    lj_err_json(sbx, LJ_ERR_BADJSON_MISSINGEND);
  }
  switch (*r) {
  case 'I':
    return lj_json_read_infinity(r, sbx, 1, o);
  case 'N':
    return lj_json_read_nan(r, sbx, o);
  default:
    break;
  }
  r = lj_json_read_number(r, sbx, o);
  if (tvisnum(o)) {
    o->n = -o->n;
  }
  return r;
}

static char *lj_json_deserialize_peek(char *r, SBufExt *sbx, uint32_t scr);

static LJ_JSON_AINLINE char *lj_json_read_array(char *r, SBufExt *sbx, uint32_t scr) {
  if (LJ_JSON_UNLIKELY(sbx->depth <= 0)) lj_err_json(sbx, LJ_ERR_BUFFER_DEPTH);
  sbx->depth--;
  GCtab *t = NULL;
  lua_State *L = sbufL(sbx);
  uint32_t asize = 0;
  r = lj_json_skip_white_space(r, sbx);
  while (LJ_JSON_LIKELY(r < sbx->w) && *r != ']') {
    uint32_t v = lj_json_scratch_pushn(L, 1);
    r = lj_json_deserialize_peek(r, sbx, v);
    r = lj_json_skip_white_space(r, sbx);
    asize++;
  }
  if (asize == 0) {
    t = lj_tab_new_ah(L, 0, 0);
  } else {
    t = lj_tab_new_ah(L, asize + 1, 0);
    cTValue *base = &scratchV(lj_json_scratch_popn(L, asize));
    TValue *array = tvref(t->array) + 1;
    if (asize < 64) {  /* An inlined loop beats memcpy for < 512 bytes. */
      for (uint32_t i = 0; i < asize; i++) {
        copyTV(L, &array[i], &base[i]);
      }
    } else {
      memcpy(array, base, asize*sizeof(TValue));
    }
  }
  settabV(L, &scratchV(scr), t);
  return r;
}

static LJ_JSON_AINLINE char *lj_json_read_object(char *r, SBufExt *sbx, uint32_t scr) {
  if (sbx->depth <= 0) lj_err_json(sbx, LJ_ERR_BUFFER_DEPTH);
  sbx->depth--;
  GCtab *t = NULL;
  lua_State *L = sbufL(sbx);
  r = lj_json_skip_white_space(r, sbx);
  uint32_t hsize = 0;
  while (LJ_JSON_LIKELY(r < sbx->w) && *r != '}') {
    uint32_t next = lj_json_scratch_pushn(L, 2);
    r = lj_json_read_key(r, sbx, next);
    next++;
    r = lj_json_skip_white_space(r, sbx);
    r = lj_json_deserialize_peek(r, sbx, next);
    r = lj_json_skip_white_space(r, sbx);
    hsize++;
  }
  if (hsize == 0) {
    t = lj_tab_new(L, 0, 0);
  } else {
    t = lj_tab_new(L, 0, hsize2hbits(hsize));
    cTValue *head = &scratchV(lj_json_scratch_popn(L, 2 * hsize));
    do {
      const GCstr *key = (const GCstr*)(uintptr_t)(head->u64);
      TValue *v = lj_tab_setstr(L, t, key);
      head++;
      copyTV(L, v, head);
      head++;
    } while (--hsize);
  }
  settabV(L, &scratchV(scr), t);
  return r;
}

static char *lj_json_deserialize_peek(char *r, SBufExt *sbx, uint32_t scr) {
  if (LJ_JSON_LIKELY(r < sbx->w)) {
    switch (*r) {
    case 'I': {
      return lj_json_read_infinity(r, sbx, 0, &scratchV(scr));
    }
    case 'N': {
      return lj_json_read_nan(r, sbx, &scratchV(scr));
    }
    case 't': {
      return lj_json_read_true(r, sbx, &scratchV(scr));
    }
    case 'f': {
      return lj_json_read_false(r, sbx, &scratchV(scr));
    }
    case 'n': {
      return lj_json_read_null(r, sbx, &scratchV(scr));
    }
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9': {
      return lj_json_read_number(r, sbx, &scratchV(scr));
    }
    case '+': {
      r++;
      return lj_json_read_number(r, sbx, &scratchV(scr));
    }
    case '-': {
      r++;
      return lj_json_read_minus(r, sbx, &scratchV(scr));
    }
    case '"': {
      r++;
      GCstr *str;
      r = lj_json_read_string(r, sbx, &str);
      setstrV(sbufL(sbx), &scratchV(scr), str);
      return r;
    }
    case '/': {
      r++;
      r = lj_json_skip_comment_space(r, sbx);
      return lj_json_deserialize_peek(r, sbx, scr);
    }
    case '[': {
      r++;
      r = lj_json_read_array(r, sbx, scr);
      if (LJ_JSON_LIKELY(r < sbx->w && *r == ']')) {
        sbx->depth++;
        r++;
        return r;
      }
      lj_err_jsonv(sbx, LJ_ERR_BADJSON_MISSINGTABEND, ']');
    }
    case '{': {
      r++;
      r = lj_json_read_object(r, sbx, scr);
      if (LJ_JSON_LIKELY(r < sbx->w && *r == '}')) {
        sbx->depth++;
        r++;
        return r;
      }
      lj_err_jsonv(sbx, LJ_ERR_BADJSON_MISSINGTABEND, '}');
    }
    default:
      lj_err_json(sbx, LJ_ERR_BADJSON_INVALIDCOMM);
    }
  }
  lj_err_json(sbx, LJ_ERR_BUFFER_EOB);
}

// JSON decoding END

/* Get serialized object from buffer. */
static char *lj_json_serialize_get(char *r, SBufExt *sbx, TValue *o)
{
  lj_json_scratch_reset();
  lua_State* L = sbufL(sbx);
  int gcrunning = lua_gc(L, LUA_GCISRUNNING, 0);
  if (gcrunning) {
    // We have to stop and later restart GC because lj_json_scratch is not properly anchored.
    // The TValues contained inside could be freed during the execution of
    // the garbage collector cycle (for example when a JIT trace is being exited).
    lua_gc(L, LUA_GCSTOP, 0);
  }
  r = lj_json_skip_white_space(r, sbx);
  if (LJ_JSON_LIKELY(r < sbx->w)) {
    uint32_t scr = lj_json_scratch_pushn(L, 1);
    r = lj_json_deserialize_peek(r, sbx, scr);
    *o = scratchV(scr);
  } else {
    GCtab *t = lj_tab_new(L, 0, hsize2hbits(0));
    settabV(L, o, t);
  }
  lj_json_scratch_free(sbufL(sbx));
  if (gcrunning) {
    lua_gc(L, LUA_GCRESTART, 0);
    lj_gc_check(L);
  }
  return r;
}

/* -- External serialization API ------------------------------------------ */

SBufExt * LJ_FASTCALL lj_serialize_json_put(SBufExt *sbx, cTValue *o)
{
  sbx->depth = LJ_SERIALIZE_DEPTH;
  sbx->w = lj_json_serialize_put(sbx->w, sbx, o);
  return sbx;
}

/* Decode from buffer. */
char * LJ_FASTCALL lj_serialize_json_get(SBufExt *sbx, TValue *o)
{
  sbx->depth = LJ_SERIALIZE_DEPTH;
  return lj_json_serialize_get(sbx->r, sbx, o);
}

/* Stand-alone encoding, borrowing from global temporary buffer. */
GCstr * LJ_FASTCALL lj_serialize_json_encode(lua_State *L, cTValue *o)
{
  SBufExt sbx;
  char *w;
  memset(&sbx, 0, sizeof(SBufExt));
  lj_bufx_set_borrow(L, &sbx, &G(L)->tmpbuf);
  sbx.depth = LJ_SERIALIZE_DEPTH;
  w = lj_json_serialize_put(sbx.w, &sbx, o);
  return lj_str_new(L, sbx.b, (size_t)(w - sbx.b));
}

/* Stand-alone decoding, copy-on-write from string. */
void lj_serialize_json_decode(lua_State *L, TValue *o, GCstr *str)
{
  SBufExt sbx;
  char *r;
  memset(&sbx, 0, sizeof(SBufExt));
  lj_bufx_set_cow(L, &sbx, strdata(str), str->len);
  /* No need to set sbx.cowref here. */
  sbx.depth = LJ_SERIALIZE_DEPTH;
  r = lj_json_serialize_get(sbx.r, &sbx, o);
  UNUSED(r);
  r = lj_json_skip_white_space(r, &sbx);
  if (r != sbx.w) lj_err_caller(L, LJ_ERR_BUFFER_LEFTOV);
}

#endif // LJ_HASBUFFER

#endif // LJ_HASJSON
