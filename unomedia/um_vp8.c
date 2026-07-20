/* ===========================================================================
 * unomedia - VP8 key-frame decoder (the lossy payload of WebP).
 *
 * Written from scratch against RFC 6386 (the VP8 Data Format and Decoding
 * Guide). The RFC embeds a BSD-licensed reference decoder; none of that code
 * is transcribed here - every function below is this file's own expression
 * of the normatively described process. The only material taken verbatim is
 * the specification's constant DATA, without which no decoder can exist:
 * the token/mode coding trees, the fixed probability tables (kf modes,
 * kf_bmode, coefficient defaults and update conditioning), the dequant
 * lookups, the zigzag and coefficient-band orders, and the two IDCT
 * cosine constants. That is format-defining data, not authorship - the
 * same footing as this repo's deflate and MP3 tables.
 *
 * ---- scope -----------------------------------------------------------------
 * Key frames only, which is total for WebP: a lossy WebP still is exactly
 * one VP8 key frame (RFC 9.1 layout), so inter prediction, motion vectors,
 * golden/altref state and probability persistence never arise. The caller
 * (um_webp.c) hands over one whole VP8 chunk in memory; this file is
 * stateless across calls.
 *
 * ---- pipeline --------------------------------------------------------------
 *   uncompressed 10-byte header -> partition-0 bool decoder ->
 *   compressed header (segmentation, loop-filter controls, partition
 *   count, quantizers, coefficient probability updates, skip flag) ->
 *   per MB, raster order: modes from partition 0, coefficient tokens from
 *   the row's token partition -> dequant -> inverse WHT (Y2) + exact
 *   integer 4x4 IDCT -> intra prediction (16x16 DC/V/H/TM, all ten 4x4
 *   B_PRED submodes, 8x8 chroma) summed with the residue -> whole-frame
 *   in-loop deblocking (normal AND simple modes, with per-segment and
 *   per-mode level adjustments) -> centred bilinear chroma upsample ->
 *   YCbCr -> RGBA.
 *
 * ---- colour ----------------------------------------------------------------
 * VP8 is BT.601 studio-range YCbCr. Verified empirically against ffmpeg's
 * reference decode: with the chroma kernel held fixed, the studio-range
 * (16..235 excursion) matrix lands ~24 dB closer than a full-range
 * interpretation (48.4 vs 24.7 dB on the 320x240 fixture), so studio is
 * what ships: the classic 1.164/1.596/0.813/0.392/2.018 set in 2.14 fixed
 * point, evaluated with libwebp's arithmetic shape so converted pixels
 * are bit-identical to the canonical decoder's. Chroma is upsampled with
 * the centred 9:3:3:1 bilinear kernel (chroma sited midway between its
 * four luma samples) - also libwebp's choice, and what ImageMagick (the
 * unomedia test suite's reference) emits. Note when comparing against
 * `ffmpeg -pix_fmt rgba` directly: swscale's default unscaled path
 * DUPLICATES chroma per 2x2 block, so on chroma-edge-heavy content it
 * disagrees with every interpolating decoder (including dwebp) by far
 * more than rounding; the decoded Y/U/V planes themselves match ffmpeg's
 * VP8 decoder bit for bit.
 *
 * ---- reconstruction notes --------------------------------------------------
 * The planes are reconstructed at macroblock granularity with a 1-pixel
 * top/left border (127 above / 129 left, RFC 12.2) and a 4-pixel right
 * pad. The pad carries the above-right pixels for B_PRED subblocks 3, 7,
 * 11 and 15, which by RFC 12.3 all use the four pixels above-right of the
 * macroblock: 127 on the top row, elsewhere the rightmost above pixel
 * replicated for the rightmost macroblock column. Excess right/bottom
 * pixels of partial macroblocks stay internal and are cropped on output.
 *
 * Every header field is distrusted: dimension guards precede allocation,
 * partition sizes are validated against the chunk, and the bool decoder
 * feeds implicit zero bytes past its partition end (which RFC 7.3 requires
 * anyway - encoders may drop trailing zeros), so truncated or corrupt
 * input decodes to garbage or fails cleanly but can never read out of
 * bounds or hang. All working state lives in one um_alloc arena, freed on
 * every path out.
 * ======================================================================== */
#include "unomedia.h"
#include "unomedia_int.h"
#include <string.h>
#include <stdint.h>

/* ---- spec constant data (RFC 6386) ---------------------------------------- */

/* zigzag scan: natural position of coefficient k (section 13.3) */
static const uint8_t kZig[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

/* coefficient position -> probability band (section 13.3) */
static const uint8_t kBand[16] = {
    0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7
};

/* extra-bit probabilities for the six large-value token categories
 * (section 13.2); base values 5, 7, 11, 19, 35, 67 */
static const uint8_t kCat1[1]  = { 159 };
static const uint8_t kCat2[2]  = { 165, 145 };
static const uint8_t kCat3[3]  = { 173, 148, 140 };
static const uint8_t kCat4[4]  = { 176, 155, 140, 135 };
static const uint8_t kCat5[5]  = { 180, 157, 141, 134, 130 };
static const uint8_t kCat6[11] = { 254, 254, 243, 230, 196, 177, 153,
                                   140, 133, 130, 129 };

/* mode numbering: 16x16 luma and chroma DC/V/H/TM = 0..3, B_PRED = 4;
 * 4x4 submodes B_DC,B_TM,B_VE,B_HE,B_LD,B_RD,B_VR,B_VL,B_HD,B_HU = 0..9 */
enum { M_DC = 0, M_V, M_H, M_TM, M_BPRED };
enum { SB_DC = 0, SB_TM, SB_VE, SB_HE, SB_LD, SB_RD, SB_VR, SB_VL,
       SB_HD, SB_HU };

/* coding trees (section 8 pair format: positive = next node index,
 * <= 0 = leaf, value -entry) and the fixed key-frame probabilities */
static const int8_t kYModeTree[8]  = { -4, 2,  4, 6,  0, -1,  -2, -3 };
static const uint8_t kYModeProb[4] = { 145, 156, 163, 128 };

static const int8_t kUvModeTree[6]  = { 0, 2,  -1, 4,  -2, -3 };
static const uint8_t kUvModeProb[3] = { 142, 114, 183 };

static const int8_t kBModeTree[18] = {
    0, 2,  -1, 4,  -2, 6,  8, 12,  -3, 10,  -5, -6,  -4, 14,
    -7, 16,  -8, -9
};

static const int8_t kSegTree[6] = { 2, 4,  0, -1,  -2, -3 };

/* de-quantization lookups (RFC 6386 section 14.1) */
static const uint16_t kDcQ[128] = {
      4,   5,   6,   7,   8,   9,  10,  10,  11,  12,  13,  14,  15,
     16,  17,  17,  18,  19,  20,  20,  21,  21,  22,  22,  23,  23,
     24,  25,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,
     36,  37,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  46,
     47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,
     60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,
     73,  74,  75,  76,  76,  77,  78,  79,  80,  81,  82,  83,  84,
     85,  86,  87,  88,  89,  91,  93,  95,  96,  98, 100, 101, 102,
    104, 106, 108, 110, 112, 114, 116, 118, 122, 124, 126, 128, 130,
    132, 134, 136, 138, 140, 143, 145, 148, 151, 154, 157,
};

static const uint16_t kAcQ[128] = {
      4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,  16,
     17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,
     30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,
     43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,
     56,  57,  58,  60,  62,  64,  66,  68,  70,  72,  74,  76,  78,
     80,  82,  84,  86,  88,  90,  92,  94,  96,  98, 100, 102, 104,
    106, 108, 110, 112, 114, 116, 119, 122, 125, 128, 131, 134, 137,
    140, 143, 146, 149, 152, 155, 158, 161, 164, 167, 170, 173, 177,
    181, 185, 189, 193, 197, 201, 205, 209, 213, 217, 221, 225, 229,
    234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284,
};

/* key-frame 4x4 submode probabilities [above][left] (RFC 6386 section 11.5) */
static const uint8_t kBModeProb[10][10][9] = {
  {
    { 231, 120,  48,  89, 115, 113, 120, 152, 112, },
    { 152, 179,  64, 126, 170, 118,  46,  70,  95, },
    { 175,  69, 143,  80,  85,  82,  72, 155, 103, },
    {  56,  58,  10, 171, 218, 189,  17,  13, 152, },
    { 144,  71,  10,  38, 171, 213, 144,  34,  26, },
    { 114,  26,  17, 163,  44, 195,  21,  10, 173, },
    { 121,  24,  80, 195,  26,  62,  44,  64,  85, },
    { 170,  46,  55,  19, 136, 160,  33, 206,  71, },
    {  63,  20,   8, 114, 114, 208,  12,   9, 226, },
    {  81,  40,  11,  96, 182,  84,  29,  16,  36, },
  },
  {
    { 134, 183,  89, 137,  98, 101, 106, 165, 148, },
    {  72, 187, 100, 130, 157, 111,  32,  75,  80, },
    {  66, 102, 167,  99,  74,  62,  40, 234, 128, },
    {  41,  53,   9, 178, 241, 141,  26,   8, 107, },
    { 104,  79,  12,  27, 217, 255,  87,  17,   7, },
    {  74,  43,  26, 146,  73, 166,  49,  23, 157, },
    {  65,  38, 105, 160,  51,  52,  31, 115, 128, },
    {  87,  68,  71,  44, 114,  51,  15, 186,  23, },
    {  47,  41,  14, 110, 182, 183,  21,  17, 194, },
    {  66,  45,  25, 102, 197, 189,  23,  18,  22, },
  },
  {
    {  88,  88, 147, 150,  42,  46,  45, 196, 205, },
    {  43,  97, 183, 117,  85,  38,  35, 179,  61, },
    {  39,  53, 200,  87,  26,  21,  43, 232, 171, },
    {  56,  34,  51, 104, 114, 102,  29,  93,  77, },
    { 107,  54,  32,  26,  51,   1,  81,  43,  31, },
    {  39,  28,  85, 171,  58, 165,  90,  98,  64, },
    {  34,  22, 116, 206,  23,  34,  43, 166,  73, },
    {  68,  25, 106,  22,  64, 171,  36, 225, 114, },
    {  34,  19,  21, 102, 132, 188,  16,  76, 124, },
    {  62,  18,  78,  95,  85,  57,  50,  48,  51, },
  },
  {
    { 193, 101,  35, 159, 215, 111,  89,  46, 111, },
    {  60, 148,  31, 172, 219, 228,  21,  18, 111, },
    { 112, 113,  77,  85, 179, 255,  38, 120, 114, },
    {  40,  42,   1, 196, 245, 209,  10,  25, 109, },
    { 100,  80,   8,  43, 154,   1,  51,  26,  71, },
    {  88,  43,  29, 140, 166, 213,  37,  43, 154, },
    {  61,  63,  30, 155,  67,  45,  68,   1, 209, },
    { 142,  78,  78,  16, 255, 128,  34, 197, 171, },
    {  41,  40,   5, 102, 211, 183,   4,   1, 221, },
    {  51,  50,  17, 168, 209, 192,  23,  25,  82, },
  },
  {
    { 125,  98,  42,  88, 104,  85, 117, 175,  82, },
    {  95,  84,  53,  89, 128, 100, 113, 101,  45, },
    {  75,  79, 123,  47,  51, 128,  81, 171,   1, },
    {  57,  17,   5,  71, 102,  57,  53,  41,  49, },
    { 115,  21,   2,  10, 102, 255, 166,  23,   6, },
    {  38,  33,  13, 121,  57,  73,  26,   1,  85, },
    {  41,  10,  67, 138,  77, 110,  90,  47, 114, },
    { 101,  29,  16,  10,  85, 128, 101, 196,  26, },
    {  57,  18,  10, 102, 102, 213,  34,  20,  43, },
    { 117,  20,  15,  36, 163, 128,  68,   1,  26, },
  },
  {
    { 138,  31,  36, 171,  27, 166,  38,  44, 229, },
    {  67,  87,  58, 169,  82, 115,  26,  59, 179, },
    {  63,  59,  90, 180,  59, 166,  93,  73, 154, },
    {  40,  40,  21, 116, 143, 209,  34,  39, 175, },
    {  57,  46,  22,  24, 128,   1,  54,  17,  37, },
    {  47,  15,  16, 183,  34, 223,  49,  45, 183, },
    {  46,  17,  33, 183,   6,  98,  15,  32, 183, },
    {  65,  32,  73, 115,  28, 128,  23, 128, 205, },
    {  40,   3,   9, 115,  51, 192,  18,   6, 223, },
    {  87,  37,   9, 115,  59,  77,  64,  21,  47, },
  },
  {
    { 104,  55,  44, 218,   9,  54,  53, 130, 226, },
    {  64,  90,  70, 205,  40,  41,  23,  26,  57, },
    {  54,  57, 112, 184,   5,  41,  38, 166, 213, },
    {  30,  34,  26, 133, 152, 116,  10,  32, 134, },
    {  75,  32,  12,  51, 192, 255, 160,  43,  51, },
    {  39,  19,  53, 221,  26, 114,  32,  73, 255, },
    {  31,   9,  65, 234,   2,  15,   1, 118,  73, },
    {  88,  31,  35,  67, 102,  85,  55, 186,  85, },
    {  56,  21,  23, 111,  59, 205,  45,  37, 192, },
    {  55,  38,  70, 124,  73, 102,   1,  34,  98, },
  },
  {
    { 102,  61,  71,  37,  34,  53,  31, 243, 192, },
    {  69,  60,  71,  38,  73, 119,  28, 222,  37, },
    {  68,  45, 128,  34,   1,  47,  11, 245, 171, },
    {  62,  17,  19,  70, 146,  85,  55,  62,  70, },
    {  75,  15,   9,   9,  64, 255, 184, 119,  16, },
    {  37,  43,  37, 154, 100, 163,  85, 160,   1, },
    {  63,   9,  92, 136,  28,  64,  32, 201,  85, },
    {  86,   6,  28,   5,  64, 255,  25, 248,   1, },
    {  56,   8,  17, 132, 137, 255,  55, 116, 128, },
    {  58,  15,  20,  82, 135,  57,  26, 121,  40, },
  },
  {
    { 164,  50,  31, 137, 154, 133,  25,  35, 218, },
    {  51, 103,  44, 131, 131, 123,  31,   6, 158, },
    {  86,  40,  64, 135, 148, 224,  45, 183, 128, },
    {  22,  26,  17, 131, 240, 154,  14,   1, 209, },
    {  83,  12,  13,  54, 192, 255,  68,  47,  28, },
    {  45,  16,  21,  91,  64, 222,   7,   1, 197, },
    {  56,  21,  39, 155,  60, 138,  23, 102, 213, },
    {  85,  26,  85,  85, 128, 128,  32, 146, 171, },
    {  18,  11,   7,  63, 144, 171,   4,   4, 246, },
    {  35,  27,  10, 146, 174, 171,  12,  26, 128, },
  },
  {
    { 190,  80,  35,  99, 180,  80, 126,  54,  45, },
    {  85, 126,  47,  87, 176,  51,  41,  20,  32, },
    { 101,  75, 128, 139, 118, 146, 116, 128,  85, },
    {  56,  41,  15, 176, 236,  85,  37,   9,  62, },
    { 146,  36,  19,  30, 171, 255,  97,  27,  20, },
    {  71,  30,  17, 119, 118, 255,  17,  18, 138, },
    { 101,  38,  60, 138,  55,  70,  43,  26, 142, },
    { 138,  45,  61,  62, 219,   1,  81, 188,  64, },
    {  32,  41,  20, 117, 151, 142,  20,  21, 163, },
    { 112,  19,  12,  61, 195, 128,  48,   4,  24, },
  },
};

/* token probability update conditioning probs (RFC 6386 section 13.4) */
static const uint8_t kCoefUpdateProb[4][8][3][11] = {
  {
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 176, 246, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 223, 241, 252, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 249, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 244, 252, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 234, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 246, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 239, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 251, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 251, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 254, 253, 255, 254, 255, 255, 255, 255, 255, 255, },
      { 250, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255, },
      { 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
  },
  {
    {
      { 217, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 225, 252, 241, 253, 255, 255, 254, 255, 255, 255, 255, },
      { 234, 250, 241, 250, 253, 255, 253, 254, 255, 255, 255, },
    },
    {
      { 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 223, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 238, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 249, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 247, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 252, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
  },
  {
    {
      { 186, 251, 250, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 234, 251, 244, 254, 255, 255, 255, 255, 255, 255, 255, },
      { 251, 251, 243, 253, 254, 255, 254, 255, 255, 255, 255, },
    },
    {
      { 255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 236, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 251, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
  },
  {
    {
      { 248, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 250, 254, 252, 254, 255, 255, 255, 255, 255, 255, 255, },
      { 248, 254, 249, 253, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 246, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 252, 254, 251, 254, 254, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 254, 252, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 248, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 253, 255, 254, 254, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 245, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 253, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 251, 253, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 252, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 252, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 249, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
    {
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
      { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, },
    },
  },
};

/* default token probabilities (RFC 6386 section 13.5) */
static const uint8_t kCoefProbDefault[4][8][3][11] = {
  {
    {
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, },
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, },
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, },
    },
    {
      { 253, 136, 254, 255, 228, 219, 128, 128, 128, 128, 128, },
      { 189, 129, 242, 255, 227, 213, 255, 219, 128, 128, 128, },
      { 106, 126, 227, 252, 214, 209, 255, 255, 128, 128, 128, },
    },
    {
      {   1,  98, 248, 255, 236, 226, 255, 255, 128, 128, 128, },
      { 181, 133, 238, 254, 221, 234, 255, 154, 128, 128, 128, },
      {  78, 134, 202, 247, 198, 180, 255, 219, 128, 128, 128, },
    },
    {
      {   1, 185, 249, 255, 243, 255, 128, 128, 128, 128, 128, },
      { 184, 150, 247, 255, 236, 224, 128, 128, 128, 128, 128, },
      {  77, 110, 216, 255, 236, 230, 128, 128, 128, 128, 128, },
    },
    {
      {   1, 101, 251, 255, 241, 255, 128, 128, 128, 128, 128, },
      { 170, 139, 241, 252, 236, 209, 255, 255, 128, 128, 128, },
      {  37, 116, 196, 243, 228, 255, 255, 255, 128, 128, 128, },
    },
    {
      {   1, 204, 254, 255, 245, 255, 128, 128, 128, 128, 128, },
      { 207, 160, 250, 255, 238, 128, 128, 128, 128, 128, 128, },
      { 102, 103, 231, 255, 211, 171, 128, 128, 128, 128, 128, },
    },
    {
      {   1, 152, 252, 255, 240, 255, 128, 128, 128, 128, 128, },
      { 177, 135, 243, 255, 234, 225, 128, 128, 128, 128, 128, },
      {  80, 129, 211, 255, 194, 224, 128, 128, 128, 128, 128, },
    },
    {
      {   1,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128, },
      { 246,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128, },
      { 255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, },
    },
  },
  {
    {
      { 198,  35, 237, 223, 193, 187, 162, 160, 145, 155,  62, },
      { 131,  45, 198, 221, 172, 176, 220, 157, 252, 221,   1, },
      {  68,  47, 146, 208, 149, 167, 221, 162, 255, 223, 128, },
    },
    {
      {   1, 149, 241, 255, 221, 224, 255, 255, 128, 128, 128, },
      { 184, 141, 234, 253, 222, 220, 255, 199, 128, 128, 128, },
      {  81,  99, 181, 242, 176, 190, 249, 202, 255, 255, 128, },
    },
    {
      {   1, 129, 232, 253, 214, 197, 242, 196, 255, 255, 128, },
      {  99, 121, 210, 250, 201, 198, 255, 202, 128, 128, 128, },
      {  23,  91, 163, 242, 170, 187, 247, 210, 255, 255, 128, },
    },
    {
      {   1, 200, 246, 255, 234, 255, 128, 128, 128, 128, 128, },
      { 109, 178, 241, 255, 231, 245, 255, 255, 128, 128, 128, },
      {  44, 130, 201, 253, 205, 192, 255, 255, 128, 128, 128, },
    },
    {
      {   1, 132, 239, 251, 219, 209, 255, 165, 128, 128, 128, },
      {  94, 136, 225, 251, 218, 190, 255, 255, 128, 128, 128, },
      {  22, 100, 174, 245, 186, 161, 255, 199, 128, 128, 128, },
    },
    {
      {   1, 182, 249, 255, 232, 235, 128, 128, 128, 128, 128, },
      { 124, 143, 241, 255, 227, 234, 128, 128, 128, 128, 128, },
      {  35,  77, 181, 251, 193, 211, 255, 205, 128, 128, 128, },
    },
    {
      {   1, 157, 247, 255, 236, 231, 255, 255, 128, 128, 128, },
      { 121, 141, 235, 255, 225, 227, 255, 255, 128, 128, 128, },
      {  45,  99, 188, 251, 195, 217, 255, 224, 128, 128, 128, },
    },
    {
      {   1,   1, 251, 255, 213, 255, 128, 128, 128, 128, 128, },
      { 203,   1, 248, 255, 255, 128, 128, 128, 128, 128, 128, },
      { 137,   1, 177, 255, 224, 255, 128, 128, 128, 128, 128, },
    },
  },
  {
    {
      { 253,   9, 248, 251, 207, 208, 255, 192, 128, 128, 128, },
      { 175,  13, 224, 243, 193, 185, 249, 198, 255, 255, 128, },
      {  73,  17, 171, 221, 161, 179, 236, 167, 255, 234, 128, },
    },
    {
      {   1,  95, 247, 253, 212, 183, 255, 255, 128, 128, 128, },
      { 239,  90, 244, 250, 211, 209, 255, 255, 128, 128, 128, },
      { 155,  77, 195, 248, 188, 195, 255, 255, 128, 128, 128, },
    },
    {
      {   1,  24, 239, 251, 218, 219, 255, 205, 128, 128, 128, },
      { 201,  51, 219, 255, 196, 186, 128, 128, 128, 128, 128, },
      {  69,  46, 190, 239, 201, 218, 255, 228, 128, 128, 128, },
    },
    {
      {   1, 191, 251, 255, 255, 128, 128, 128, 128, 128, 128, },
      { 223, 165, 249, 255, 213, 255, 128, 128, 128, 128, 128, },
      { 141, 124, 248, 255, 255, 128, 128, 128, 128, 128, 128, },
    },
    {
      {   1,  16, 248, 255, 255, 128, 128, 128, 128, 128, 128, },
      { 190,  36, 230, 255, 236, 255, 128, 128, 128, 128, 128, },
      { 149,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128, },
    },
    {
      {   1, 226, 255, 128, 128, 128, 128, 128, 128, 128, 128, },
      { 247, 192, 255, 128, 128, 128, 128, 128, 128, 128, 128, },
      { 240, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128, },
    },
    {
      {   1, 134, 252, 255, 255, 128, 128, 128, 128, 128, 128, },
      { 213,  62, 250, 255, 255, 128, 128, 128, 128, 128, 128, },
      {  55,  93, 255, 128, 128, 128, 128, 128, 128, 128, 128, },
    },
    {
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, },
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, },
      { 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, },
    },
  },
  {
    {
      { 202,  24, 213, 235, 186, 191, 220, 160, 240, 175, 255, },
      { 126,  38, 182, 232, 169, 184, 228, 174, 255, 187, 128, },
      {  61,  46, 138, 219, 151, 178, 240, 170, 255, 216, 128, },
    },
    {
      {   1, 112, 230, 250, 199, 191, 247, 159, 255, 255, 128, },
      { 166, 109, 228, 252, 211, 215, 255, 174, 128, 128, 128, },
      {  39,  77, 162, 232, 172, 180, 245, 178, 255, 255, 128, },
    },
    {
      {   1,  52, 220, 246, 198, 199, 249, 220, 255, 255, 128, },
      { 124,  74, 191, 243, 183, 193, 250, 221, 255, 255, 128, },
      {  24,  71, 130, 219, 154, 170, 243, 182, 255, 255, 128, },
    },
    {
      {   1, 182, 225, 249, 219, 240, 255, 224, 128, 128, 128, },
      { 149, 150, 226, 252, 216, 205, 255, 171, 128, 128, 128, },
      {  28, 108, 170, 242, 183, 194, 254, 223, 255, 255, 128, },
    },
    {
      {   1,  81, 230, 252, 204, 203, 255, 192, 128, 128, 128, },
      { 123, 102, 209, 247, 188, 196, 255, 233, 128, 128, 128, },
      {  20,  95, 153, 243, 164, 173, 255, 203, 128, 128, 128, },
    },
    {
      {   1, 222, 248, 255, 216, 213, 128, 128, 128, 128, 128, },
      { 168, 175, 246, 252, 235, 205, 255, 255, 128, 128, 128, },
      {  47, 116, 215, 255, 211, 212, 255, 255, 128, 128, 128, },
    },
    {
      {   1, 121, 236, 253, 212, 214, 255, 255, 128, 128, 128, },
      { 141,  84, 213, 252, 201, 202, 255, 219, 128, 128, 128, },
      {  42,  80, 160, 240, 162, 185, 255, 205, 128, 128, 128, },
    },
    {
      {   1,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128, },
      { 244,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128, },
      { 238,   1, 255, 128, 128, 128, 128, 128, 128, 128, 128, },
    },
  },
};

/* ---- boolean decoder (RFC 6386 section 7) ---------------------------------
 * `val` keeps the arithmetic-coding window left-justified in 32 bits;
 * `cnt` is the number of loaded-but-unconsumed bits minus 8. Once the
 * partition is exhausted the shifts feed in zero bits, which is exactly
 * the implicit zero padding the format specifies. */
typedef struct {
    const unsigned char *p, *end;
    uint32_t val;
    uint32_t range;
    int32_t  cnt;
} vbd;

static void bd_fill(vbd *b)
{
    while (b->cnt <= 16 && b->p < b->end) {
        b->val |= (uint32_t)*b->p++ << (16 - b->cnt);
        b->cnt += 8;
    }
}

static void bd_init(vbd *b, const unsigned char *p, long n)
{
    b->p = p; b->end = p + (n > 0 ? n : 0);
    b->val = 0; b->range = 255; b->cnt = -8;
    bd_fill(b);
}

static int bd_bit(vbd *b, int prob)
{
    uint32_t split = 1 + (((b->range - 1) * (uint32_t)prob) >> 8);
    int bit = 0;
    if ((b->val >> 24) >= split) {
        b->range -= split;
        b->val   -= split << 24;
        bit = 1;
    } else
        b->range = split;
    while (b->range < 128) {
        b->range <<= 1;
        b->val   <<= 1;
        b->cnt--;
    }
    if (b->cnt <= 16) bd_fill(b);
    return bit;
}

static int bd_flag(vbd *b) { return bd_bit(b, 128); }

static int bd_lit(vbd *b, int n)        /* unsigned literal, MSB first */
{
    int v = 0;
    while (n--) v = (v << 1) | bd_bit(b, 128);
    return v;
}

static int bd_sig(vbd *b, int n)        /* magnitude then sign flag */
{
    int v = bd_lit(b, n);
    return bd_flag(b) ? -v : v;
}

static int bd_tree(vbd *b, const int8_t *tree, const uint8_t *prob)
{
    int i = 0;
    do i = tree[i + bd_bit(b, prob[i >> 1])]; while (i > 0);
    return -i;
}

/* ---- decoder state -------------------------------------------------------- */
#define MB_LNZ 9        /* left/above nonzero flags: 4 Y, 2 U, 2 V, 1 Y2 */

/* mbinfo packing */
#define MBI_SEG(i)   ((i) & 3)
#define MBI_BPRED    0x04
#define MBI_NZ       0x08

typedef struct {
    int w, h, mbw, mbh;
    long ys, cs;                     /* luma / chroma plane strides       */
    uint8_t *y, *u, *v;              /* plane origins (inside borders)    */
    uint8_t *above_nz;               /* mbw * MB_LNZ                      */
    uint8_t *above_bm;               /* mbw * 4 bottom-row subblock modes */
    uint8_t *mbinfo;                 /* mbw * mbh                         */
    uint8_t left_nz[MB_LNZ];
    uint8_t left_bm[4];

    vbd hd;                          /* partition 0 after the header      */
    vbd tp[8];                       /* token partitions                  */
    int ntp;

    int seg_on, seg_map, seg_abs;
    int seg_q[4], seg_lf[4];
    uint8_t seg_prob[3];
    int simple, flevel, sharp;
    int lfa_on;
    int lf_ref0, lf_bpred;           /* the two deltas key frames can use */
    int skip_on;
    uint8_t skip_prob;

    uint16_t dq[4][6];               /* [seg][ydc yac y2dc y2ac uvdc uvac] */
    uint8_t probs[4][8][3][11];      /* live coefficient probabilities    */

    int16_t coef[25 * 16];           /* current MB: 16 Y, 4 U, 4 V, Y2    */
    uint8_t bnz[25];                 /* per-block any-nonzero flags       */
} vp8d;

/* ---- uncompressed frame header (RFC 6386 section 9.1) --------------------- */
static int vp8_frame_tag(const unsigned char *buf, long n,
                         int *w, int *h, long *part0)
{
    uint32_t tag;
    int ww, hh;
    if (n < 10) { um_set_error("VP8: truncated header"); return 0; }
    tag = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) | ((uint32_t)buf[2] << 16);
    if (tag & 1) { um_set_error("VP8: not a key frame"); return 0; }
    if (((tag >> 1) & 7) > 3) { um_set_error("VP8: reserved version"); return 0; }
    if (buf[3] != 0x9d || buf[4] != 0x01 || buf[5] != 0x2a) {
        um_set_error("VP8: bad start code"); return 0;
    }
    ww = (buf[6] | (buf[7] << 8)) & 0x3fff;      /* upscaling fields ignored */
    hh = (buf[8] | (buf[9] << 8)) & 0x3fff;
    if (ww < 1 || hh < 1 || ww > UM_MAX_DIM || hh > UM_MAX_DIM ||
        (long)ww * hh > UM_MAX_PIXELS) {
        um_set_error("VP8: dimensions out of range"); return 0;
    }
    *part0 = (long)(tag >> 5);
    if (*part0 < 1 || *part0 > n - 10) {
        um_set_error("VP8: truncated partition"); return 0;
    }
    *w = ww; *h = hh;
    return 1;
}

int um_vp8_dims(const unsigned char *buf, long n, int *w, int *h)
{
    long p0;
    return vp8_frame_tag(buf, n, w, h, &p0);
}

/* ---- compressed frame header (RFC 6386 sections 9.2-9.11) ----------------- */
static int vp8_header(vp8d *d, const unsigned char *buf, long n, long part0)
{
    vbd *b = &d->hd;
    int i, j, k, t, nlog;
    long off, rem;

    bd_init(b, buf + 10, part0);
    bd_flag(b);                                  /* colour space (0 only)    */
    bd_flag(b);                                  /* clamping type            */

    d->seg_on = bd_flag(b);
    d->seg_map = 0; d->seg_abs = 0;
    d->seg_prob[0] = d->seg_prob[1] = d->seg_prob[2] = 255;
    for (i = 0; i < 4; i++) d->seg_q[i] = d->seg_lf[i] = 0;
    if (d->seg_on) {
        int upd_data;
        d->seg_map = bd_flag(b);
        upd_data = bd_flag(b);
        if (upd_data) {
            d->seg_abs = bd_flag(b);
            for (i = 0; i < 4; i++)
                if (bd_flag(b)) d->seg_q[i] = bd_sig(b, 7);
            for (i = 0; i < 4; i++)
                if (bd_flag(b)) d->seg_lf[i] = bd_sig(b, 6);
        }
        if (d->seg_map)
            for (i = 0; i < 3; i++)
                if (bd_flag(b)) d->seg_prob[i] = (uint8_t)bd_lit(b, 8);
    }

    d->simple = bd_flag(b);
    d->flevel = bd_lit(b, 6);
    d->sharp  = bd_lit(b, 3);
    d->lfa_on = bd_flag(b);
    d->lf_ref0 = d->lf_bpred = 0;
    if (d->lfa_on && bd_flag(b)) {               /* mode_ref_lf_delta_update */
        for (i = 0; i < 4; i++)                  /* per-reference deltas     */
            if (bd_flag(b)) {
                int v = bd_sig(b, 6);
                if (i == 0) d->lf_ref0 = v;      /* INTRA_FRAME              */
            }
        for (i = 0; i < 4; i++)                  /* per-mode deltas          */
            if (bd_flag(b)) {
                int v = bd_sig(b, 6);
                if (i == 0) d->lf_bpred = v;     /* B_PRED                   */
            }
    }

    nlog = bd_lit(b, 2);                         /* token partition count    */
    d->ntp = 1 << nlog;
    off = 10 + part0 + 3 * ((long)d->ntp - 1);
    if (off > n) { um_set_error("VP8: truncated partition"); return 0; }
    rem = n - off;
    for (i = 0; i < d->ntp; i++) {
        long sz = rem;
        if (i < d->ntp - 1) {
            const unsigned char *s = buf + 10 + part0 + 3 * (long)i;
            sz = (long)s[0] | ((long)s[1] << 8) | ((long)s[2] << 16);
            if (sz > rem) { um_set_error("VP8: truncated partition"); return 0; }
        }
        bd_init(&d->tp[i], buf + off, sz);
        off += sz; rem -= sz;
    }

    {                                            /* quantizer indices        */
        int yac  = bd_lit(b, 7);
        int ydc  = bd_flag(b) ? bd_sig(b, 4) : 0;
        int y2dc = bd_flag(b) ? bd_sig(b, 4) : 0;
        int y2ac = bd_flag(b) ? bd_sig(b, 4) : 0;
        int uvdc = bd_flag(b) ? bd_sig(b, 4) : 0;
        int uvac = bd_flag(b) ? bd_sig(b, 4) : 0;
        int s, nseg = d->seg_on ? 4 : 1;
        for (s = 0; s < nseg; s++) {
            int q = yac;
            long v;
            if (d->seg_on)
                q = d->seg_abs ? d->seg_q[s] : q + d->seg_q[s];
#define QCL(x) ((x) < 0 ? 0 : ((x) > 127 ? 127 : (x)))
            d->dq[s][0] = kDcQ[QCL(q + ydc)];
            d->dq[s][1] = kAcQ[QCL(q)];
            d->dq[s][2] = (uint16_t)(kDcQ[QCL(q + y2dc)] * 2);
            v = (long)kAcQ[QCL(q + y2ac)] * 155 / 100;
            d->dq[s][3] = (uint16_t)(v < 8 ? 8 : v);
            v = kDcQ[QCL(q + uvdc)];
            d->dq[s][4] = (uint16_t)(v > 132 ? 132 : v);
            d->dq[s][5] = kAcQ[QCL(q + uvac)];
#undef QCL
        }
    }

    bd_flag(b);                                  /* refresh_entropy_probs    */

    memcpy(d->probs, kCoefProbDefault, sizeof(d->probs));
    for (i = 0; i < 4; i++)
        for (j = 0; j < 8; j++)
            for (k = 0; k < 3; k++)
                for (t = 0; t < 11; t++)
                    if (bd_bit(b, kCoefUpdateProb[i][j][k][t]))
                        d->probs[i][j][k][t] = (uint8_t)bd_lit(b, 8);

    d->skip_on = bd_flag(b);
    d->skip_prob = d->skip_on ? (uint8_t)bd_lit(b, 8) : 0;
    return 1;
}

/* ---- coefficient tokens (RFC 6386 sections 13.2-13.3) --------------------- */
static int vp8_cat(vbd *b, const uint8_t *p, int n, int base)
{
    int v = 0;
    while (n--) v = (v << 1) | bd_bit(b, *p++);
    return base + v;
}

/* Decode one 4x4 residue block. probs = the plane's [band][ctx][11] table,
 * first = starting coefficient, ctx = 0..2 from the neighbours' nonzero
 * flags. Coefficients land dequantized in out[] (natural order). Returns 1
 * if the block has any nonzero coefficient. */
static int vp8_block(vbd *b, const uint8_t (*probs)[3][11], int first,
                     int ctx, int dqdc, int dqac, int16_t *out)
{
    int i = first, nz = 0, skip_eob = 0;
    while (i < 16) {
        const uint8_t *p = probs[kBand[i]][ctx];
        int v;
        if (!skip_eob && !bd_bit(b, p[0])) break;         /* end of block   */
        if (!bd_bit(b, p[1])) {                           /* DCT_0          */
            skip_eob = 1; ctx = 0; i++;
            continue;
        }
        skip_eob = 0;
        if (!bd_bit(b, p[2]))
            v = 1;
        else if (!bd_bit(b, p[3])) {                      /* 2, 3, 4        */
            if (!bd_bit(b, p[4])) v = 2;
            else                  v = 3 + bd_bit(b, p[5]);
        } else if (!bd_bit(b, p[6])) {                    /* cat 1-2        */
            if (!bd_bit(b, p[7])) v = vp8_cat(b, kCat1, 1, 5);
            else                  v = vp8_cat(b, kCat2, 2, 7);
        } else if (!bd_bit(b, p[8])) {                    /* cat 3-4        */
            if (!bd_bit(b, p[9])) v = vp8_cat(b, kCat3, 3, 11);
            else                  v = vp8_cat(b, kCat4, 4, 19);
        } else {                                          /* cat 5-6        */
            if (!bd_bit(b, p[10])) v = vp8_cat(b, kCat5, 5, 35);
            else                   v = vp8_cat(b, kCat6, 11, 67);
        }
        ctx = v > 1 ? 2 : 1;
        if (bd_flag(b)) v = -v;
        /* 16-bit truncating store, as the format prescribes */
        out[kZig[i]] = (int16_t)(uint16_t)((long)v * (i ? dqac : dqdc));
        nz = 1;
        i++;
    }
    return nz;
}

/* Decode all residue blocks of one macroblock into d->coef / d->bnz,
 * maintaining the nonzero contexts. Returns 1 if any block has coeffs. */
static int vp8_residuals(vp8d *d, vbd *tb, int mx, int has_y2, int seg)
{
    uint8_t *anz = d->above_nz + (long)mx * MB_LNZ, *lnz = d->left_nz;
    const uint16_t *dq = d->dq[seg];
    int b, nz, mbnz = 0;

    memset(d->coef, 0, sizeof(d->coef));
    memset(d->bnz, 0, sizeof(d->bnz));
    if (has_y2) {
        nz = vp8_block(tb, d->probs[1], 0, anz[8] + lnz[8],
                       dq[2], dq[3], d->coef + 24 * 16);
        d->bnz[24] = (uint8_t)nz;
        anz[8] = lnz[8] = (uint8_t)nz; mbnz |= nz;
    }
    for (b = 0; b < 16; b++) {
        nz = vp8_block(tb, d->probs[has_y2 ? 0 : 3], has_y2 ? 1 : 0,
                       anz[b & 3] + lnz[b >> 2],
                       dq[0], dq[1], d->coef + b * 16);
        d->bnz[b] = (uint8_t)nz;
        anz[b & 3] = lnz[b >> 2] = (uint8_t)nz; mbnz |= nz;
    }
    for (b = 0; b < 4; b++) {
        nz = vp8_block(tb, d->probs[2], 0,
                       anz[4 + (b & 1)] + lnz[4 + (b >> 1)],
                       dq[4], dq[5], d->coef + (16 + b) * 16);
        d->bnz[16 + b] = (uint8_t)nz;
        anz[4 + (b & 1)] = lnz[4 + (b >> 1)] = (uint8_t)nz; mbnz |= nz;
    }
    for (b = 0; b < 4; b++) {
        nz = vp8_block(tb, d->probs[2], 0,
                       anz[6 + (b & 1)] + lnz[6 + (b >> 1)],
                       dq[4], dq[5], d->coef + (20 + b) * 16);
        d->bnz[20 + b] = (uint8_t)nz;
        anz[6 + (b & 1)] = lnz[6 + (b >> 1)] = (uint8_t)nz; mbnz |= nz;
    }
    return mbnz;
}

/* ---- inverse transforms (RFC 6386 sections 14.3-14.4) --------------------- */
#define VP8_C1 20091            /* sqrt(2)*cos(pi/8) - 1, 16-bit fixed */
#define VP8_C2 35468            /* sqrt(2)*sin(pi/8),     16-bit fixed */

#define T16(x) ((int16_t)(uint16_t)(x))         /* reference 16-bit stores */

static void vp8_iwht(const int16_t *in, int16_t *out)
{
    int16_t m[16];
    int i;
    for (i = 0; i < 4; i++) {
        int a = in[i] + in[12 + i], b = in[4 + i] + in[8 + i];
        int c = in[4 + i] - in[8 + i], e = in[i] - in[12 + i];
        m[i]      = T16(a + b);
        m[4 + i]  = T16(c + e);
        m[8 + i]  = T16(a - b);
        m[12 + i] = T16(e - c);
    }
    for (i = 0; i < 4; i++) {
        const int16_t *r = m + i * 4;
        int a = r[0] + r[3], b = r[1] + r[2];
        int c = r[1] - r[2], e = r[0] - r[3];
        out[i * 4]     = T16((a + b + 3) >> 3);
        out[i * 4 + 1] = T16((c + e + 3) >> 3);
        out[i * 4 + 2] = T16((a - b + 3) >> 3);
        out[i * 4 + 3] = T16((e - c + 3) >> 3);
    }
}

static uint8_t px_clamp(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* IDCT the block and sum it into the 4x4 of pixels at dst */
static void vp8_idct_add(const int16_t *in, uint8_t *dst, long stride)
{
    int16_t m[16];
    int i;
    for (i = 0; i < 4; i++) {
        int t0 = in[i] + in[8 + i];
        int t1 = in[i] - in[8 + i];
        int t2 = ((in[4 + i] * VP8_C2) >> 16)
               - (in[12 + i] + ((in[12 + i] * VP8_C1) >> 16));
        int t3 = (in[4 + i] + ((in[4 + i] * VP8_C1) >> 16))
               + ((in[12 + i] * VP8_C2) >> 16);
        m[i]      = T16(t0 + t3);
        m[4 + i]  = T16(t1 + t2);
        m[8 + i]  = T16(t1 - t2);
        m[12 + i] = T16(t0 - t3);
    }
    for (i = 0; i < 4; i++) {
        const int16_t *r = m + i * 4;
        uint8_t *o = dst + i * stride;
        int t0 = r[0] + r[2];
        int t1 = r[0] - r[2];
        int t2 = ((r[1] * VP8_C2) >> 16) - (r[3] + ((r[3] * VP8_C1) >> 16));
        int t3 = (r[1] + ((r[1] * VP8_C1) >> 16)) + ((r[3] * VP8_C2) >> 16);
        o[0] = px_clamp(o[0] + ((t0 + t3 + 4) >> 3));
        o[1] = px_clamp(o[1] + ((t1 + t2 + 4) >> 3));
        o[2] = px_clamp(o[2] + ((t1 - t2 + 4) >> 3));
        o[3] = px_clamp(o[3] + ((t0 - t3 + 4) >> 3));
    }
}

/* ---- intra prediction (RFC 6386 section 12) ------------------------------- */

/* whole-block DC/V/H/TM over an n x n block sitting in the plane; the
 * border rows/columns already hold 127/129 at frame edges */
static void pred_wall(uint8_t *dst, long st, int n, int mode,
                      int up, int left)
{
    int r, c;
    switch (mode) {
    case M_DC: {
        int v = 128;
        if (up | left) {
            int sum = 0, shf = (n == 16) ? 4 : 3;
            if (up)   for (c = 0; c < n; c++) sum += dst[c - st];
            if (left) for (r = 0; r < n; r++) sum += dst[r * st - 1];
            if (up & left) shf++;
            v = (sum + (1 << (shf - 1))) >> shf;
        }
        for (r = 0; r < n; r++) memset(dst + r * st, v, (size_t)n);
        break;
    }
    case M_V:
        for (r = 0; r < n; r++) memcpy(dst + r * st, dst - st, (size_t)n);
        break;
    case M_H:
        for (r = 0; r < n; r++)
            memset(dst + r * st, dst[r * st - 1], (size_t)n);
        break;
    default: {                                   /* M_TM */
        int p = dst[-st - 1];
        for (r = 0; r < n; r++) {
            int l = dst[r * st - 1];
            for (c = 0; c < n; c++)
                dst[r * st + c] = px_clamp(l + dst[c - st] - p);
        }
        break;
    }
    }
}

/* 4x4 subblock prediction. e[] holds the 13 edge pixels:
 * e[0..3] = left column bottom-to-top, e[4] = above-left corner,
 * e[5..12] = above row plus above-right. */
#define AV2(x, y)    (uint8_t)(((x) + (y) + 1) >> 1)
#define AV3(x, y, z) (uint8_t)(((x) + 2 * (y) + (z) + 2) >> 2)

static void pred_sub(uint8_t *dst, long st, int mode, const uint8_t *e)
{
    const uint8_t *A = e + 5;                    /* A[-1] = corner          */
    uint8_t L[4], B[16];
    int r, c;
    L[0] = e[3]; L[1] = e[2]; L[2] = e[1]; L[3] = e[0];

    switch (mode) {
    case SB_DC: {
        int v = A[0] + A[1] + A[2] + A[3] + L[0] + L[1] + L[2] + L[3];
        memset(B, (v + 4) >> 3, 16);
        break;
    }
    case SB_TM:
        for (r = 0; r < 4; r++)
            for (c = 0; c < 4; c++)
                B[r * 4 + c] = px_clamp(L[r] + A[c] - A[-1]);
        break;
    case SB_VE:
        for (c = 0; c < 4; c++)
            B[c] = B[4 + c] = B[8 + c] = B[12 + c] =
                AV3(A[c - 1], A[c], A[c + 1]);
        break;
    case SB_HE:
        for (r = 0; r < 3; r++)
            B[r * 4] = B[r * 4 + 1] = B[r * 4 + 2] = B[r * 4 + 3] =
                AV3(r ? L[r - 1] : A[-1], L[r], L[r + 1]);
        B[12] = B[13] = B[14] = B[15] = AV3(L[2], L[3], L[3]);
        break;
    case SB_LD:
        B[0] = AV3(A[0], A[1], A[2]);
        B[1] = B[4] = AV3(A[1], A[2], A[3]);
        B[2] = B[5] = B[8] = AV3(A[2], A[3], A[4]);
        B[3] = B[6] = B[9] = B[12] = AV3(A[3], A[4], A[5]);
        B[7] = B[10] = B[13] = AV3(A[4], A[5], A[6]);
        B[11] = B[14] = AV3(A[5], A[6], A[7]);
        B[15] = AV3(A[6], A[7], A[7]);
        break;
    case SB_RD:
        B[12] = AV3(e[0], e[1], e[2]);
        B[13] = B[8] = AV3(e[1], e[2], e[3]);
        B[14] = B[9] = B[4] = AV3(e[2], e[3], e[4]);
        B[15] = B[10] = B[5] = B[0] = AV3(e[3], e[4], e[5]);
        B[11] = B[6] = B[1] = AV3(e[4], e[5], e[6]);
        B[7] = B[2] = AV3(e[5], e[6], e[7]);
        B[3] = AV3(e[6], e[7], e[8]);
        break;
    case SB_VR:
        B[12] = AV3(e[1], e[2], e[3]);
        B[8] = AV3(e[2], e[3], e[4]);
        B[13] = B[4] = AV3(e[3], e[4], e[5]);
        B[9] = B[0] = AV2(e[4], e[5]);
        B[14] = B[5] = AV3(e[4], e[5], e[6]);
        B[10] = B[1] = AV2(e[5], e[6]);
        B[15] = B[6] = AV3(e[5], e[6], e[7]);
        B[11] = B[2] = AV2(e[6], e[7]);
        B[7] = AV3(e[6], e[7], e[8]);
        B[3] = AV2(e[7], e[8]);
        break;
    case SB_VL:
        B[0] = AV2(A[0], A[1]);
        B[4] = AV3(A[0], A[1], A[2]);
        B[8] = B[1] = AV2(A[1], A[2]);
        B[5] = B[12] = AV3(A[1], A[2], A[3]);
        B[9] = B[2] = AV2(A[2], A[3]);
        B[13] = B[6] = AV3(A[2], A[3], A[4]);
        B[10] = B[3] = AV2(A[3], A[4]);
        B[14] = B[7] = AV3(A[3], A[4], A[5]);
        B[11] = AV3(A[4], A[5], A[6]);
        B[15] = AV3(A[5], A[6], A[7]);
        break;
    case SB_HD:
        B[12] = AV2(e[0], e[1]);
        B[13] = AV3(e[0], e[1], e[2]);
        B[8] = B[14] = AV2(e[1], e[2]);
        B[9] = B[15] = AV3(e[1], e[2], e[3]);
        B[10] = B[4] = AV2(e[2], e[3]);
        B[11] = B[5] = AV3(e[2], e[3], e[4]);
        B[6] = B[0] = AV2(e[3], e[4]);
        B[7] = B[1] = AV3(e[3], e[4], e[5]);
        B[2] = AV3(e[4], e[5], e[6]);
        B[3] = AV3(e[5], e[6], e[7]);
        break;
    default:                                     /* SB_HU */
        B[0] = AV2(L[0], L[1]);
        B[1] = AV3(L[0], L[1], L[2]);
        B[2] = B[4] = AV2(L[1], L[2]);
        B[3] = B[5] = AV3(L[1], L[2], L[3]);
        B[6] = B[8] = AV2(L[2], L[3]);
        B[7] = B[9] = AV3(L[2], L[3], L[3]);
        B[10] = B[11] = B[12] = B[13] = B[14] = B[15] = L[3];
        break;
    }
    for (r = 0; r < 4; r++)
        memcpy(dst + r * st, B + r * 4, 4);
}

/* ---- macroblock reconstruction -------------------------------------------- */
static void vp8_recon_mb(vp8d *d, int mx, int my, int ym, int uvm,
                         const uint8_t *bm, int skip)
{
    uint8_t *yd = d->y + (long)my * 16 * d->ys + (long)mx * 16;
    uint8_t *ud = d->u + (long)my * 8 * d->cs + (long)mx * 8;
    uint8_t *vd = d->v + (long)my * 8 * d->cs + (long)mx * 8;
    int b;

    if (ym == M_BPRED) {
        uint8_t ar[4];                           /* MB's above-right pixels */
        memcpy(ar, yd - d->ys + 16, 4);
        for (b = 0; b < 16; b++) {
            long o = (long)(b >> 2) * 4 * d->ys + (b & 3) * 4;
            uint8_t *sd = yd + o;
            uint8_t e[13];
            int i;
            for (i = 0; i < 4; i++) e[i] = sd[(3 - i) * d->ys - 1];
            e[4] = sd[-d->ys - 1];
            if ((b & 3) == 3) {
                for (i = 0; i < 4; i++) e[5 + i] = sd[-d->ys + i];
                memcpy(e + 9, ar, 4);
            } else
                for (i = 0; i < 8; i++) e[5 + i] = sd[-d->ys + i];
            pred_sub(sd, d->ys, bm[b], e);
            if (!skip && d->bnz[b])
                vp8_idct_add(d->coef + b * 16, sd, d->ys);
        }
    } else {
        pred_wall(yd, d->ys, 16, ym, my > 0, mx > 0);
        if (!skip) {
            if (d->bnz[24]) {
                int16_t dc[16];
                vp8_iwht(d->coef + 24 * 16, dc);
                for (b = 0; b < 16; b++) d->coef[b * 16] = dc[b];
            }
            for (b = 0; b < 16; b++)
                if (d->bnz[b] || d->coef[b * 16])
                    vp8_idct_add(d->coef + b * 16,
                                 yd + (long)(b >> 2) * 4 * d->ys
                                    + (b & 3) * 4, d->ys);
        }
    }

    pred_wall(ud, d->cs, 8, uvm, my > 0, mx > 0);
    pred_wall(vd, d->cs, 8, uvm, my > 0, mx > 0);
    if (!skip)
        for (b = 0; b < 4; b++) {
            long o = (long)(b >> 1) * 4 * d->cs + (b & 1) * 4;
            if (d->bnz[16 + b])
                vp8_idct_add(d->coef + (16 + b) * 16, ud + o, d->cs);
            if (d->bnz[20 + b])
                vp8_idct_add(d->coef + (20 + b) * 16, vd + o, d->cs);
        }
}

/* ---- in-loop deblocking filter (RFC 6386 section 15) ---------------------- */
static int s8(int v) { return v < -128 ? -128 : (v > 127 ? 127 : v); }
static int iabs(int v) { return v < 0 ? -v : v; }

/* the shared edge adjustment; returns the amount taken off q0 */
static int lf_adjust(uint8_t *p, long s, int outer)
{
    int p1 = p[-2 * s] - 128, p0 = p[-s] - 128;
    int q0 = p[0] - 128,      q1 = p[s] - 128;
    int a  = s8((outer ? s8(p1 - q1) : 0) + 3 * (q0 - p0));
    int f2 = s8(a + 3) >> 3;
    int f1 = s8(a + 4) >> 3;
    p[-s] = (uint8_t)(s8(p0 + f2) + 128);
    p[0]  = (uint8_t)(s8(q0 - f1) + 128);
    return f1;
}

static int lf_ok(const uint8_t *p, long s, int E, int I)
{
    int p3 = p[-4 * s], p2 = p[-3 * s], p1 = p[-2 * s], p0 = p[-s];
    int q0 = p[0], q1 = p[s], q2 = p[2 * s], q3 = p[3 * s];
    return iabs(p0 - q0) * 2 + iabs(p1 - q1) / 2 <= E &&
           iabs(p3 - p2) <= I && iabs(p2 - p1) <= I && iabs(p1 - p0) <= I &&
           iabs(q3 - q2) <= I && iabs(q2 - q1) <= I && iabs(q1 - q0) <= I;
}

static int lf_hev(const uint8_t *p, long s, int H)
{
    return iabs(p[-2 * s] - p[-s]) > H || iabs(p[s] - p[0]) > H;
}

static void lf_simple(uint8_t *p, long s, int E)
{
    if (iabs(p[-s] - p[0]) * 2 + iabs(p[-2 * s] - p[s]) / 2 <= E)
        lf_adjust(p, s, 1);
}

static void lf_sub(uint8_t *p, long s, int E, int I, int H)
{
    int hev, a;
    if (!lf_ok(p, s, E, I)) return;
    hev = lf_hev(p, s, H);
    a = (lf_adjust(p, s, hev) + 1) >> 1;
    if (!hev) {
        p[s]      = (uint8_t)(s8(p[s] - 128 - a) + 128);
        p[-2 * s] = (uint8_t)(s8(p[-2 * s] - 128 + a) + 128);
    }
}

static void lf_mbedge(uint8_t *p, long s, int E, int I, int H)
{
    if (!lf_ok(p, s, E, I)) return;
    if (lf_hev(p, s, H))
        lf_adjust(p, s, 1);
    else {
        int p2 = p[-3 * s] - 128, p1 = p[-2 * s] - 128, p0 = p[-s] - 128;
        int q0 = p[0] - 128, q1 = p[s] - 128, q2 = p[2 * s] - 128;
        int w = s8(s8(p1 - q1) + 3 * (q0 - p0));
        int a = s8((27 * w + 63) >> 7);
        p[-s] = (uint8_t)(s8(p0 + a) + 128);
        p[0]  = (uint8_t)(s8(q0 - a) + 128);
        a = s8((18 * w + 63) >> 7);
        p[-2 * s] = (uint8_t)(s8(p1 + a) + 128);
        p[s]      = (uint8_t)(s8(q1 - a) + 128);
        a = s8((9 * w + 63) >> 7);
        p[-3 * s] = (uint8_t)(s8(p2 + a) + 128);
        p[2 * s]  = (uint8_t)(s8(q2 - a) + 128);
    }
}

static void vp8_filter_frame(vp8d *d)
{
    int mx, my, i, e;
    for (my = 0; my < d->mbh; my++)
        for (mx = 0; mx < d->mbw; mx++) {
            int info = d->mbinfo[(long)my * d->mbw + mx];
            int lvl = d->flevel, I, Emb, Esb, H, inner;
            uint8_t *yp = d->y + (long)my * 16 * d->ys + (long)mx * 16;
            uint8_t *up = d->u + (long)my * 8 * d->cs + (long)mx * 8;
            uint8_t *vp = d->v + (long)my * 8 * d->cs + (long)mx * 8;

            if (d->seg_on) {
                int s = MBI_SEG(info);
                lvl = d->seg_abs ? d->seg_lf[s] : lvl + d->seg_lf[s];
                lvl = lvl < 0 ? 0 : (lvl > 63 ? 63 : lvl);
            }
            if (d->lfa_on) {
                lvl += d->lf_ref0;               /* intra frame delta      */
                if (info & MBI_BPRED) lvl += d->lf_bpred;
                lvl = lvl < 0 ? 0 : (lvl > 63 ? 63 : lvl);
            }
            if (!lvl) continue;

            I = lvl;
            if (d->sharp) {
                I >>= d->sharp > 4 ? 2 : 1;
                if (I > 9 - d->sharp) I = 9 - d->sharp;
            }
            if (!I) I = 1;
            Emb = (lvl + 2) * 2 + I;
            Esb = lvl * 2 + I;
            H = lvl >= 40 ? 2 : (lvl >= 15 ? 1 : 0);    /* key frame */
            inner = (info & (MBI_NZ | MBI_BPRED)) != 0;

            if (d->simple) {                     /* luma only              */
                if (mx)
                    for (i = 0; i < 16; i++)
                        lf_simple(yp + i * d->ys, 1, Emb);
                if (inner)
                    for (e = 4; e < 16; e += 4)
                        for (i = 0; i < 16; i++)
                            lf_simple(yp + i * d->ys + e, 1, Esb);
                if (my)
                    for (i = 0; i < 16; i++)
                        lf_simple(yp + i, d->ys, Emb);
                if (inner)
                    for (e = 4; e < 16; e += 4)
                        for (i = 0; i < 16; i++)
                            lf_simple(yp + (long)e * d->ys + i, d->ys, Esb);
                continue;
            }

            if (mx) {
                for (i = 0; i < 16; i++)
                    lf_mbedge(yp + i * d->ys, 1, Emb, I, H);
                for (i = 0; i < 8; i++) {
                    lf_mbedge(up + i * d->cs, 1, Emb, I, H);
                    lf_mbedge(vp + i * d->cs, 1, Emb, I, H);
                }
            }
            if (inner) {
                for (e = 4; e < 16; e += 4)
                    for (i = 0; i < 16; i++)
                        lf_sub(yp + i * d->ys + e, 1, Esb, I, H);
                for (i = 0; i < 8; i++) {
                    lf_sub(up + i * d->cs + 4, 1, Esb, I, H);
                    lf_sub(vp + i * d->cs + 4, 1, Esb, I, H);
                }
            }
            if (my) {
                for (i = 0; i < 16; i++)
                    lf_mbedge(yp + i, d->ys, Emb, I, H);
                for (i = 0; i < 8; i++) {
                    lf_mbedge(up + i, d->cs, Emb, I, H);
                    lf_mbedge(vp + i, d->cs, Emb, I, H);
                }
            }
            if (inner) {
                for (e = 4; e < 16; e += 4)
                    for (i = 0; i < 16; i++)
                        lf_sub(yp + (long)e * d->ys + i, d->ys, Esb, I, H);
                for (i = 0; i < 8; i++) {
                    lf_sub(up + 4 * d->cs + i, d->cs, Esb, I, H);
                    lf_sub(vp + 4 * d->cs + i, d->cs, Esb, I, H);
                }
            }
        }
}

/* ---- chroma upsample + BT.601 studio-range YCbCr -> RGBA ------------------ */
static void vp8_output(const vp8d *d, um_px *dst)
{
    int x, y;
    int cw = (d->w + 1) >> 1, ch = (d->h + 1) >> 1;
    for (y = 0; y < d->h; y++) {
        const uint8_t *yr = d->y + (long)y * d->ys;
        int cy = y >> 1;
        int oy = (y & 1) ? cy + 1 : cy - 1;
        const uint8_t *un, *uf, *vn, *vf;
        um_px *o = dst + (long)y * d->w;
        if (oy < 0) oy = 0;
        if (oy >= ch) oy = ch - 1;
        un = d->u + (long)cy * d->cs; uf = d->u + (long)oy * d->cs;
        vn = d->v + (long)cy * d->cs; vf = d->v + (long)oy * d->cs;
        for (x = 0; x < d->w; x++) {
            int cx = x >> 1;
            int ox = (x & 1) ? cx + 1 : cx - 1;
            int Y = yr[x], U, V, r, g, b;
            if (ox < 0) ox = 0;
            if (ox >= cw) ox = cw - 1;
            U = (9 * un[cx] + 3 * un[ox] + 3 * uf[cx] + uf[ox] + 8) >> 4;
            V = (9 * vn[cx] + 3 * vn[ox] + 3 * vf[cx] + vf[ox] + 8) >> 4;
            /* studio-range BT.601 in 2.14 fixed point (1.164 Y' +
             * 1.596 Cr, etc.), evaluated as (x*c)>>8 partials then >>6
             * with the rounding half folded into the bias - the same
             * arithmetic shape libwebp uses, so output pixels are
             * bit-identical to the canonical decoder */
            r = ((19077 * Y) >> 8) + ((26149 * V) >> 8) - 14234;
            g = ((19077 * Y) >> 8) - ((6419 * U) >> 8) - ((13320 * V) >> 8)
              + 8708;
            b = ((19077 * Y) >> 8) + ((33050 * U) >> 8) - 17685;
            r = r < 0 ? 0 : (r > 16383 ? 255 : r >> 6);
            g = g < 0 ? 0 : (g > 16383 ? 255 : g >> 6);
            b = b < 0 ? 0 : (b > 16383 ? 255 : b >> 6);
            o[x] = UM_PX(r, g, b, 0xFF);
        }
    }
}

/* ---- the frame decoder ---------------------------------------------------- */
int um_vp8_decode(const unsigned char *buf, long n, um_px *dst)
{
    int w, h, mbw, mbh, mx, my, i;
    long part0, ys, cs, ybytes, cbytes, need;
    unsigned char *arena;
    vp8d *d;

    if (!vp8_frame_tag(buf, n, &w, &h, &part0)) return 0;
    mbw = (w + 15) >> 4; mbh = (h + 15) >> 4;
    ys = (long)mbw * 16 + 8;                     /* 1 left border, 4 right
                                                    pad, 3 spare           */
    cs = (long)mbw * 8 + 8;
    ybytes = ys * ((long)mbh * 16 + 1);          /* + top border row       */
    cbytes = cs * ((long)mbh * 8 + 1);

    need = (long)sizeof(vp8d) + ybytes + 2 * cbytes
         + (long)mbw * MB_LNZ + (long)mbw * 4 + (long)mbw * mbh + 8;
    arena = um_alloc((unsigned long)need);
    if (!arena) { um_set_error("VP8: out of memory"); return 0; }
    memset(arena, 0, (size_t)need);

    d = (vp8d *)(void *)arena;
    d->w = w; d->h = h; d->mbw = mbw; d->mbh = mbh; d->ys = ys; d->cs = cs;
    {
        unsigned char *m = arena + sizeof(vp8d);
        d->y = m + ys + 1;        m += ybytes;    /* origin inside borders  */
        d->u = m + cs + 1;        m += cbytes;
        d->v = m + cs + 1;        m += cbytes;
        d->above_nz = m;          m += (long)mbw * MB_LNZ;
        d->above_bm = m;          m += (long)mbw * 4;
        d->mbinfo = m;
    }

    if (!vp8_header(d, buf, n, part0)) { um_free(arena); return 0; }

    /* borders: 127 across the full row above the frame (this includes the
     * above-left corner and the above-right pad), 129 down the left edge */
    memset(d->y - ys - 1, 127, (size_t)ys);
    memset(d->u - cs - 1, 127, (size_t)cs);
    memset(d->v - cs - 1, 127, (size_t)cs);
    for (i = 0; i < mbh * 16; i++) d->y[(long)i * ys - 1] = 129;
    for (i = 0; i < mbh * 8; i++) {
        d->u[(long)i * cs - 1] = 129;
        d->v[(long)i * cs - 1] = 129;
    }

    for (my = 0; my < mbh; my++) {
        vbd *tb = &d->tp[my & (d->ntp - 1)];
        memset(d->left_nz, 0, sizeof(d->left_nz));
        memset(d->left_bm, 0, sizeof(d->left_bm));
        for (mx = 0; mx < mbw; mx++) {
            uint8_t bm[16];
            int seg = 0, skip = 0, ym, uvm, has_y2, mbnz = 0, b;

            if (d->seg_map)
                seg = bd_tree(&d->hd, kSegTree, d->seg_prob);
            if (d->skip_on)
                skip = bd_bit(&d->hd, d->skip_prob);

            ym = bd_tree(&d->hd, kYModeTree, kYModeProb);
            if (ym == M_BPRED) {
                for (b = 0; b < 16; b++) {
                    int a = (b < 4) ? d->above_bm[(long)mx * 4 + b]
                                    : bm[b - 4];
                    int l = (b & 3) ? bm[b - 1] : d->left_bm[b >> 2];
                    bm[b] = (uint8_t)bd_tree(&d->hd, kBModeTree,
                                             kBModeProb[a][l]);
                }
            } else {
                /* implied submode, for ensuing context only (11.3) */
                static const uint8_t imp[4] = { SB_DC, SB_VE, SB_HE, SB_TM };
                memset(bm, imp[ym], sizeof(bm));
            }
            memcpy(d->above_bm + (long)mx * 4, bm + 12, 4);
            for (b = 0; b < 4; b++) d->left_bm[b] = bm[b * 4 + 3];
            uvm = bd_tree(&d->hd, kUvModeTree, kUvModeProb);
            has_y2 = ym != M_BPRED;

            if (skip) {
                uint8_t *anz = d->above_nz + (long)mx * MB_LNZ;
                memset(d->coef, 0, sizeof(d->coef));
                memset(d->bnz, 0, sizeof(d->bnz));
                memset(anz, 0, 8);
                memset(d->left_nz, 0, 8);
                if (has_y2) anz[8] = d->left_nz[8] = 0;
            } else
                mbnz = vp8_residuals(d, tb, mx, has_y2, seg);

            vp8_recon_mb(d, mx, my, ym, uvm, bm, skip);
            d->mbinfo[(long)my * mbw + mx] = (uint8_t)
                (seg | (ym == M_BPRED ? MBI_BPRED : 0) | (mbnz ? MBI_NZ : 0));
        }
        /* extend the rightmost pixels into the pad: the above-right
         * source for the next row's rightmost B_PRED macroblock */
        for (i = 0; i < 16; i++) {
            uint8_t *r = d->y + ((long)my * 16 + i) * ys + (long)mbw * 16;
            memset(r, r[-1], 4);
        }
    }

    if (d->flevel)                 /* reference decoders gate on the frame
                                      level, segment overrides and all    */
        vp8_filter_frame(d);

    vp8_output(d, dst);
    um_free(arena);
    return 1;
}
