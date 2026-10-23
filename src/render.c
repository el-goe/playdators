#include "draw_style.h"
#include "render.h"
#include "platform.h"
#include "util/adjacency_generator.h"
#include "util/radixsort.h"
#include "util/pixel_ops.h"
#include <string.h>
#include <stdlib.h>   // _byteswap_ulong on MSVC

// Parts of the scanline rasterizer were originally based on
// - mini3d from Playdate SDK examples. Created by Dave Hayden on 10/20/15. Copyright © 2015 Panic, Inc. All rights reserved.
// But probably almost everything is replaced by now :)

FORCE_INLINE void write_masked_black(uint32_t* p, uint32_t mask)
{
	*p = (*p & ~mask); // " | (color & mask)" but color is always black
}

// A byte swap the compiler can see through. mathlib.h's swap() is an asm
// volatile REV, which the compiler may neither move, merge nor fold into a
// constant -- in the fill below that is two or three extra register moves a
// span, and a mask known at compile time still paying for a REV at run time.
// __builtin_bswap32 compiles to the same REV where one is needed.
FORCE_INLINE uint32_t fx_swap(uint32_t v)
{
#if defined(_MSC_VER)
	return _byteswap_ulong(v);
#else
	return __builtin_bswap32(v);
#endif
}

// Clears [x1, x2) on one row, clipped to the screen: one row of a wire line.
//
// The same masks as before, built the way the triangle fill builds its own
// (fx_span_pattern, below): no division or modulo by 32, no branch on which
// of the two end masks is empty, and the end word written through its own
// pointer. Inlined, since it is one row of the line loop and the call cost as
// much as a short fragment.
FORCE_INLINE void draw_black_fragment(uint32_t* row, int x1, int x2)
{
	if (x2 < 0 || x1 >= SCREEN_X)
		return;
	if (x1 < 0)
		x1 = 0;
	if (x2 > SCREEN_X)
		x2 = SCREEN_X;
	if (x1 >= x2)
		return;   // x1 == x2 cleared nothing before either

	// Operate on 32 bits at a time. Unsigned, and without a shift by 32: the
	// signed forms were undefined at startbit 0 and endbit 31.
	uint32_t* p = row + (x1 >> 5);
	uint32_t* const last = row + (x2 >> 5);
	uint32_t m = 0xFFFFFFFFu >> (x1 & 31);
	if (p != last)
	{
		if (x1 & 31)
			write_masked_black(p++, fx_swap(m));
		while (p < last)
			*p++ = 0;
		if ((x2 & 31) == 0)
			return;
		m = 0xFFFFFFFFu;
	}
	write_masked_black(last, fx_swap(m & ~(0xFFFFFFFFu >> (x2 & 31))));
}

static inline int32_t slope(float x1, float y1, float x2, float y2, int shift)
{
	float dx = x2-x1;
	float dy = y2-y1;
	
	if (dy < 1)
		return (int32_t)(dx * (1 << shift));
	else
		return (int32_t)(dx / dy * (1 << shift));
}

static void draw_black_line(uint8_t* bitmap, int rowstride, const float3* p1, const float3* p2)
{
	if (p1->y > p2->y)
	{
		const float3* tmp = p1;
		p1 = p2;
		p2 = tmp;
	}

	int y = (int)p1->y;
	int endy = (int)p2->y;
	
	if (y >= SCREEN_Y || endy < 0 || min2f(p1->x, p2->x) >= SCREEN_X || max2f(p1->x, p2->x) < 0)
		return;
	
	int32_t x = (int32_t)(p1->x * (1<<16));
	int32_t dx = slope(p1->x, p1->y, p2->x, p2->y, 16);
	float py = p1->y;
	
	if (y < 0)
	{
		x += (int32_t)(-p1->y * dx);
		y = 0;
		py = 0;
	}

	int32_t x1 = (int32_t)(x + dx * (y+1-py));

	while (y <= endy)
	{
		if (y == endy)
			x1 = (int32_t)(p2->x * (1<<16));
		
		if (dx < 0)
			draw_black_fragment((uint32_t*)&bitmap[y * rowstride], x1 >> 16, (x >> 16) + 1);
		else
			draw_black_fragment((uint32_t*)&bitmap[y * rowstride], x >> 16, (x1 >> 16) + 1);

		if (++y == SCREEN_Y)
			break;

		x = x1;
		x1 += dx;
	}
}

// --------------------------------------------------------------------------
// Overall scene rendering


void scene_init(Scene* scene)
{
	memset(scene, 0, sizeof(*scene));
	scene_setCamera(scene, f3(0, 0, 0), f3(0, 0, 1), 1.0f, f3(0, 1, 0));
	scene_setLight(scene, f3(0, -1, 0));

	scene_setCenter(scene, 0.5f, 0.5f);
}

void scene_shutdown(Scene* scene)
{
	plat_free(scene->tmp_view_z);
	plat_free(scene->tmp_view_xy);
	plat_free(scene->tmp_vert_codes);
	plat_free(scene->tmp_fix_points);
	plat_free(scene->tmp_order_table1);
	plat_free(scene->tmp_order_table2);
	plat_free(scene->instances);
	plat_free(scene->tmp_inst_order_table1);
	plat_free(scene->tmp_inst_order_table2);
	plat_free(scene->line_buf);
	plat_free(scene->tmp_clip_pts);
	plat_free(scene->tmp_clip_src);
	plat_free(scene->tmp_scene_tris);
	plat_free(scene->tmp_scene_order1);
	plat_free(scene->tmp_scene_order2);
}

void scene_setLight(Scene* scene, float3 light)
{
	scene->light = v3_normalize(light);
	scene->light.y = -scene->light.y; // compensate for view space being flipped vertically
}

void scene_setCenter(Scene* scene, float x, float y)
{
	scene->centerx = x;
	scene->centery = y;
}

void scene_setCamera(Scene* scene, float3 origin, float3 lookAt, float scale, float3 up)
{
	scene->scale = 240 * scale;
	xform m = xform_look_at(origin, lookAt, up);
	scene->camera_xform = m;
	m.ay = v3_neg(m.ay);
	scene->view_xform = xform_invert(&m);
}

typedef uint8_t Pattern[8];

static Pattern patterns[] =
{
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x80, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00 },
	{ 0x88, 0x00, 0x00, 0x00, 0x88, 0x00, 0x00, 0x00 },
	{ 0x88, 0x00, 0x20, 0x00, 0x88, 0x00, 0x02, 0x00 },
	{ 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 },
	{ 0xa8, 0x00, 0x22, 0x00, 0x8a, 0x00, 0x22, 0x00 },
	{ 0xaa, 0x00, 0x22, 0x00, 0xaa, 0x00, 0x22, 0x00 },
	{ 0xaa, 0x00, 0xa2, 0x00, 0xaa, 0x00, 0x2a, 0x00 },
	{ 0xaa, 0x00, 0xaa, 0x00, 0xaa, 0x00, 0xaa, 0x00 },
	{ 0xaa, 0x40, 0xaa, 0x00, 0xaa, 0x04, 0xaa, 0x00 },
	{ 0xaa, 0x44, 0xaa, 0x00, 0xaa, 0x44, 0xaa, 0x00 },
	{ 0xaa, 0x44, 0xaa, 0x10, 0xaa, 0x44, 0xaa, 0x01 },
	{ 0xaa, 0x44, 0xaa, 0x11, 0xaa, 0x44, 0xaa, 0x11 },
	{ 0xaa, 0x54, 0xaa, 0x11, 0xaa, 0x45, 0xaa, 0x11 },
	{ 0xaa, 0x55, 0xaa, 0x11, 0xaa, 0x55, 0xaa, 0x11 },
	{ 0xaa, 0x55, 0xaa, 0x51, 0xaa, 0x55, 0xaa, 0x15 },
	{ 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55 },
	{ 0xba, 0x55, 0xaa, 0x55, 0xab, 0x55, 0xaa, 0x55 },
	{ 0xbb, 0x55, 0xaa, 0x55, 0xbb, 0x55, 0xaa, 0x55 },
	{ 0xbb, 0x55, 0xea, 0x55, 0xbb, 0x55, 0xae, 0x55 },
	{ 0xbb, 0x55, 0xee, 0x55, 0xbb, 0x55, 0xee, 0x55 },
	{ 0xfb, 0x55, 0xee, 0x55, 0xbf, 0x55, 0xee, 0x55 },
	{ 0xff, 0x55, 0xee, 0x55, 0xff, 0x55, 0xee, 0x55 },
	{ 0xff, 0x55, 0xfe, 0x55, 0xff, 0x55, 0xef, 0x55 },
	{ 0xff, 0x55, 0xff, 0x55, 0xff, 0x55, 0xff, 0x55 },
	{ 0xff, 0x55, 0xff, 0xd5, 0xff, 0x55, 0xff, 0x5d },
	{ 0xff, 0x55, 0xff, 0xdd, 0xff, 0x55, 0xff, 0xdd },
	{ 0xff, 0x75, 0xff, 0xdd, 0xff, 0x57, 0xff, 0xdd },
	{ 0xff, 0x77, 0xff, 0xdd, 0xff, 0x77, 0xff, 0xdd },
	{ 0xff, 0x77, 0xff, 0xfd, 0xff, 0x77, 0xff, 0xdf },
	{ 0xff, 0x77, 0xff, 0xff, 0xff, 0x77, 0xff, 0xff },
	{ 0xff, 0xf7, 0xff, 0xff, 0xff, 0x7f, 0xff, 0xff },
	{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }
};

// --------------------------------------------------------------------------
// The fixed-point rasterizer
//
// Chris Hecker's edge walk, exactly as rasterizer_scanline.h's tri_edge_init
// and tri_edge_step have it -- same 28.4 inputs, same Ceil28_4, same
// floor-division DDA, so the same pixels -- but taking the points already in
// 28.4 (see SceneTri) instead of converting three floats per edge per draw.
// It lives here rather than in rasterizer_scanline.h so that this project's
// changes to the engine stay in render.c and render.h.
//
// What is cheaper, and why it matters on the M7:
//   * no float work at all per triangle: the conversions happened once, when
//     the triangle was queued;
//   * Ceil28_4 is a shift. The original branches on the sign and divides;
//     (v + 15) >> 4 is the same floor for every int, sign included;
//   * the span fill works in shifts and whole words -- no division or modulo
//     by 32, and the row pointer and pattern row step instead of being
//     recomputed from Y each line.
// --------------------------------------------------------------------------

FORCE_INLINE int fx_ceil28_4(int v)
{
	// ceil(v / 16) for a 28.4 value: floor((v - 1 + 16) / 16), and an
	// arithmetic shift IS floor division by 16 for negative values too.
	return (v + 15) >> 4;
}

// One span of a dither pattern, [x, endx), on one row. 0 <= x < endx <= 400.
// Bit order is the display's: the leftmost pixel of a word is the top bit of
// its first byte, so masks are built big-endian and byte-swapped.
//
// The end word is written through `last` rather than through the pointer the
// middle loop walked, and its mask is only built once it is needed: the two
// are equal, but written this way neither the walked pointer nor the end mask
// is live across the loop, which is what kept the row loop around this in
// registers. A span inside one word and the end of a longer one share the
// last read-modify-write.
FORCE_INLINE void fx_span_pattern(uint32_t* row, int x, int endx, uint32_t color)
{
	uint32_t* p = row + (x >> 5);
	uint32_t* const last = row + (endx >> 5);
	uint32_t m = 0xFFFFFFFFu >> (x & 31);

	if (p != last)
	{
		if (x & 31)
		{
			const uint32_t ms = fx_swap(m);
			*p = (*p & ~ms) | (color & ms);
			++p;
		}
		while (p < last)
			*p++ = color;
		if ((endx & 31) == 0)
			return;
		m = 0xFFFFFFFFu;
	}
	// One word, or the end of a longer span. In the one-word case endx cannot
	// sit on a word boundary, so the mask is not empty.
	m = fx_swap(m & ~(0xFFFFFFFFu >> (endx & 31)));
	*last = (*last & ~m) | (color & m);
}

// One span of blue noise: each pixel is set where the triangle's level is
// above the noise at that pixel. As draw_scanline_bluenoise has it.
//
// FOUR PIXELS AT A TIME. The noise is a byte a pixel, and the compare was a
// byte load, a compare and a conditional OR a pixel, with a test for the end
// of the word on every one -- ten instructions a pixel. Four noise bytes are
// one aligned word, and the compare can be done on all four at once:
//
//   * spread into 16-bit lanes, even pixels in one word and odd in another,
//     each lane has eight bits of headroom;
//   * (0xFF + level) - noise in a lane is at least 0x100 exactly when
//     level > noise, and at most 0x1FE, so bit 8 of the lane is the answer
//     and no lane borrows from its neighbour;
//   * the four answer bits, one per byte lane after a shift, are gathered
//     into a nibble -- pixel 0 at the top, the display's order -- by one
//     multiply, whose partial products land on four distinct bits.
//
// About two and a half instructions a pixel. Only whole groups of four that
// touch the span are read: group starts are multiples of four in a row whose
// stride (400) is too, so every load is aligned, and none reaches past the
// row's own last pixel. What a group brings in from outside [x, endx) is
// masked off exactly as before. Same pixels, bit for bit.
FORCE_INLINE uint32_t fx_bn_nibble(uint32_t w, uint32_t a2)
{
	const uint32_t e = (a2 - (w & 0x00FF00FFu)) & 0x01000100u;          // pixels 0, 2
	const uint32_t o = (a2 - ((w >> 8) & 0x00FF00FFu)) & 0x01000100u;   // pixels 1, 3
	// 0 or 1 in each byte lane, pixel k in lane k; then lane k to bit 31 - k.
	return (((e >> 8) | o) * 0x80402010u) >> 28;
}

FORCE_INLINE void fx_span_bluenoise(uint32_t* row, const uint8_t* noise_row,
                                    int x, int endx, uint8_t level)
{
	const uint32_t a2 = (0xFFu + level) * 0x00010001u;
	uint32_t* p = row + (x >> 5);
	uint32_t* const last = row + ((endx - 1) >> 5);
	const uint8_t* g = noise_row + (x & ~3);                 // first group
	const uint8_t* const gend = noise_row + ((endx + 3) & ~3);
	// The groups are walked in one run, a word's worth at a time, each new
	// nibble shifted in at the bottom: a word that ends on its last group
	// then has every group in place, and only the span's last word needs
	// lining up. Masks are needed at the two ends only.
	uint32_t m = 0xFFFFFFFFu >> (x & 31);
	for (;;)
	{
		const uint8_t* const wend = noise_row + ((p - row) << 5) + 32;   // this word's groups end
		const uint8_t* const stop = (gend < wend) ? gend : wend;
		uint32_t c = 0;   // pixel 0 at bit 31, once lined up
		do
		{
			uint32_t w;
			memcpy(&w, g, 4);
			c = (c << 4) | fx_bn_nibble(w, a2);
			g += 4;
		} while (g < stop);
		if (p == last)
		{
			c <<= (wend - stop);   // 4 bits per missing group of 4 bytes
			if (endx & 31)
				m &= ~(0xFFFFFFFFu >> (endx & 31));
			const uint32_t ms = fx_swap(m);
			*p = (*p & ~ms) | (fx_swap(c) & ms);
			return;
		}
		if (m == 0xFFFFFFFFu)
			*p = fx_swap(c);
		else
		{
			const uint32_t ms = fx_swap(m);
			*p = (*p & ~ms) | (fx_swap(c) & ms);
		}
		++p;
		m = 0xFFFFFFFFu;
	}
}

#if defined(_MSC_VER)
#  define FX_NOINLINE __declspec(noinline)
#else
#  define FX_NOINLINE __attribute__((noinline))
#endif

// THE DITHER PATTERNS AS WORDS. A pattern row is one byte repeated across
// the screen; the fill writes 32 pixels at a time, so it wants that byte
// already in all four lanes of a word. Built once, 33 x 8 words.
static uint32_t s_pat32[33][8];
static bool     s_pat32_ready;

static void fx_init_patterns(void)
{
	if (s_pat32_ready)
		return;
	for (int i = 0; i < 33; ++i)
		for (int r = 0; r < 8; ++r)
			s_pat32[i][r] = (uint32_t)patterns[i][r] * 0x01010101u;
	s_pat32_ready = true;
}

// An empty asm that claims to change a register, so the compiler stops
// treating it as an induction variable. Left to it, GCC works out each
// edge's, the row's and y's value at the end of the loop in closed form --
// start plus rows times step -- for the bottom half to carry on from, and
// keeps every starting value and the row count on the stack all through the
// loop to do it, when they are in registers the whole time anyway. Costs no
// instruction.
#if defined(__GNUC__)
#  define FX_OPAQUE(v) __asm__ ("" : "+r"(v))
#else
#  define FX_OPAQUE(v) ((void)0)
#endif

// BOTH HALVES OF A PATTERN TRIANGLE, in one call: the top half between the
// long edge and the first short one, then the bottom half with the second
// short edge swapped in. The long edge carries straight on from one half to
// the other. One call a triangle instead of one a half -- triangles here
// average four and a half rows, and a call with its register saves and its
// arguments off the stack was a good part of what a half cost.
//
// AND WITHOUT THE CLAMPS when the caller can show they never bite. Every
// edge walk starts at a vertex and steps by a slope truncated towards zero,
// and its start is floored to 1/65536 of a pixel; so at every row it lies
// between its two vertices' x, give or take that 1/65536 below. Its ceiling
// -- the pixel column -- is then between the ceilings of the two vertex x.
// With all three corners at 28.4 x in [0, 6400), that is [0, 400]: the
// clamps to the screen are no-ops, and the copy below without them draws the
// same pixels with three instructions a row fewer.
FORCE_INLINE void fx_tri_pattern_body(uint8_t* row, const int rowstride, int y,
                                      int h, int h2,
                                      uint32_t lF, int32_t lS, uint32_t rF, int32_t rS,
                                      const bool long_left, uint32_t bF, int32_t bS,
                                      const uint32_t* pat, const bool clip)
{
	for (;;)
	{
		for (; h > 0; --h)
		{
			int x = (int32_t)lF >> 16;
			int endx = (int32_t)rF >> 16;
			if (clip)
			{
				if (x < 0) x = 0;
				if (endx > SCREEN_X) endx = SCREEN_X;
			}
			if (x < endx)
				fx_span_pattern((uint32_t*)row, x, endx, pat[y & 7]);
			lF += (uint32_t)lS;
			rF += (uint32_t)rS;
			row += rowstride;
			++y;
			FX_OPAQUE(lF);
			FX_OPAQUE(rF);
			FX_OPAQUE(row);
			FX_OPAQUE(y);
		}
		if (h2 <= 0)
			return;
		// The bottom half: the second short edge replaces the first.
		h = h2;
		h2 = 0;
		if (long_left) { rF = bF; rS = bS; }
		else           { lF = bF; lS = bS; }
	}
}

// For the Playdate's own 52-byte row the stride is a constant, which the loop
// needs: with it a variable, the loop wanted one register more than the M7
// has and reloaded things from the stack every row. Every frame this game
// draws is a Playdate frame, so the generic copy is there for the API's sake.
static FX_NOINLINE void fx_tri_pattern_pd_clip(uint8_t* row, int y, int h, int h2,
                            uint32_t lF, int32_t lS, uint32_t rF, int32_t rS,
                            int long_left, uint32_t bF, int32_t bS, const uint32_t* pat)
{
	fx_tri_pattern_body(row, SCREEN_STRIDE_BYTES, y, h, h2, lF, lS, rF, rS,
	                    long_left != 0, bF, bS, pat, true);
}

static FX_NOINLINE void fx_tri_pattern_pd_inside(uint8_t* row, int y, int h, int h2,
                            uint32_t lF, int32_t lS, uint32_t rF, int32_t rS,
                            int long_left, uint32_t bF, int32_t bS, const uint32_t* pat)
{
	fx_tri_pattern_body(row, SCREEN_STRIDE_BYTES, y, h, h2, lF, lS, rF, rS,
	                    long_left != 0, bF, bS, pat, false);
}

static FX_NOINLINE void fx_tri_pattern(uint8_t* row, int rowstride, int y, int h, int h2,
                            uint32_t lF, int32_t lS, uint32_t rF, int32_t rS,
                            int long_left, uint32_t bF, int32_t bS, const uint32_t* pat)
{
	fx_tri_pattern_body(row, rowstride, y, h, h2, lF, lS, rF, rS,
	                    long_left != 0, bF, bS, pat, true);
}

// THE ROWS OF ONE HALF OF A BLUE-NOISE TRIANGLE, between two edges. Out of
// line so that the loop is the only thing live and stays in registers; the
// edges come in by value and are not handed back (fx_fill_triangle_body starts the
// long edge afresh for the second half, the same number), and the body is
// instantiated for the Playdate's 52-byte row as a constant as well as for
// any stride, for the reasons given at fx_tri_pattern_body.
FORCE_INLINE void fx_rows_bluenoise_body(uint8_t* row, const int rowstride, int y, int height,
                                         uint32_t lF, int32_t lS, uint32_t rF, int32_t rS,
                                         const uint8_t* noise, uint8_t level)
{
	for (; height > 0; --height)
	{
		int x = (int32_t)lF >> 16;
		int endx = (int32_t)rF >> 16;
		if (x < 0) x = 0;
		if (endx > SCREEN_X) endx = SCREEN_X;
		if (x < endx)
			fx_span_bluenoise((uint32_t*)row, noise + y * SCREEN_X, x, endx, level);
		lF += (uint32_t)lS;
		rF += (uint32_t)rS;
		row += rowstride;
		++y;
	}
}

static FX_NOINLINE void fx_rows_bluenoise_pd(uint8_t* row, int y, int height,
                              uint32_t lF, int32_t lS, uint32_t rF, int32_t rS,
                              const uint8_t* noise, uint8_t level)
{
	fx_rows_bluenoise_body(row, SCREEN_STRIDE_BYTES, y, height, lF, lS, rF, rS, noise, level);
}

static FX_NOINLINE void fx_rows_bluenoise(uint8_t* row, int rowstride, int y, int height,
                              uint32_t lF, int32_t lS, uint32_t rF, int32_t rS,
                              const uint8_t* noise, uint8_t level)
{
	fx_rows_bluenoise_body(row, rowstride, y, height, lF, lS, rF, rS, noise, level);
}

// THE EDGE WALK, IN 16.16.
//
// This was Chris Hecker's exact DDA -- an integer x, a step, and a remainder
// carried against a denominator -- which costs two divisions per edge to set
// up and five live registers and a compare-and-branch per edge per row to
// walk. Triangles here average four and a half rows, so the walk and its
// register pressure were most of what a row cost: the M7 build spilled half
// of the loop's state to the stack.
//
// In 16.16 an edge is two registers and an add. It is not exact: the slope is
// truncated to 1/65536 of a pixel, so after n rows the edge can be n/65536 of
// a pixel off where the exact walk had it -- under a hundredth of a pixel for
// the tallest edge the guard band allows -- and a pixel centre that the true
// edge passes within that distance of can land on the other side. That is
// the whole of the difference, and it is a few pixels a frame.
//
// It fits: the guard band keeps every projected x in [-512, 912], so a slope
// in 28.4 over 28.4 is at most 22,784 * 65536 before the divide, inside an
// int32; and the start point is one 32x32->64 multiply, which is a single
// SMULL on the M7.
//
// The same raster rule as before: rows whose centre is inside [top, bottom),
// pixels whose centre is inside [left, right) -- the points arrive already
// offset by half a pixel, so "centre inside" is ceil of the edge's x. The
// 0xFFFF bias makes that ceil a plain shift.
//
// An edge's slope and its x at the centre of row Y, for the walk above:
// 16.16, the start biased by 0xFFFF so that x >> 16 is the ceil. dN > 0.
FORCE_INLINE int32_t fx_slope(int32_t dM, int32_t dN)
{
	return (int32_t)((uint32_t)dM * 65536u) / dN;
}

FORCE_INLINE uint32_t fx_start(int32_t topX, int32_t topY, int Y, int32_t S)
{
	// The top vertex, plus the slope times how far below it row Y's centre
	// is -- in sixteenths, so the product is sixteen times too big.
	const int64_t down = (int64_t)(16 * Y - topY) * S;
	return (uint32_t)(topX * 4096 + (int32_t)(down >> 4)) + 0xFFFFu;
}

// Fills one triangle. `bluenoise` picks the fill; `shade` is its pattern
// index or its level accordingly.
//
// Three rows matter -- the first centre at or below the top vertex, the
// middle one, the bottom -- and every edge's extent is a difference of two
// of them, so each is worked out once. The long edge (top to bottom) is set
// up always; each short edge only if its half has a row in it, which for a
// small triangle is often only one of them.
// Out of line, and once per fill: inlined into scene_draw's draw loop, the
// setup shared that function's registers with everything else in it and
// spilled most of its values, and one copy for both fills is allocated for
// the worse of the two. Each wrapper below has the register file to itself,
// for the price of a call a triangle.
FORCE_INLINE void fx_fill_triangle_body(uint8_t* bitmap, int rowstride, const SceneTri* t,
                                        const bool bluenoise)
{
	const int32_t X0 = t->x[0], X1 = t->x[1], X2 = t->x[2];
	const int32_t Y0 = t->y[0], Y1 = t->y[1], Y2 = t->y[2];
	int32_t tx, ty, mx, my, bx, by;
	bool mid_is_left;

	// tri_sort_vertices, on the fixed-point values. Which side the middle
	// vertex is on follows from the winding -- every queued triangle faces
	// the camera -- and so from the order the sort found them in.
	if (Y0 < Y1)
	{
		if (Y2 < Y0)      { tx = X2; ty = Y2; mx = X0; my = Y0; bx = X1; by = Y1; mid_is_left = false; }
		else if (Y1 < Y2) { tx = X0; ty = Y0; mx = X1; my = Y1; bx = X2; by = Y2; mid_is_left = false; }
		else              { tx = X0; ty = Y0; mx = X2; my = Y2; bx = X1; by = Y1; mid_is_left = true;  }
	}
	else
	{
		if (Y2 < Y1)      { tx = X2; ty = Y2; mx = X1; my = Y1; bx = X0; by = Y0; mid_is_left = true;  }
		else if (Y0 < Y2) { tx = X1; ty = Y1; mx = X0; my = Y0; bx = X2; by = Y2; mid_is_left = true;  }
		else              { tx = X1; ty = Y1; mx = X2; my = Y2; bx = X0; by = Y0; mid_is_left = false; }
	}

	const int row_top = clamp_i(fx_ceil28_4(ty), 0, SCREEN_Y);
	const int row_mid = clamp_i(fx_ceil28_4(my), 0, SCREEN_Y);
	const int row_bot = clamp_i(fx_ceil28_4(by), 0, SCREEN_Y);
	if (row_bot <= row_top)
		return;           // no scanline centre inside it at all

	// The long edge, top to bottom, across both halves.
	const int32_t sl = fx_slope(bx - tx, by - ty);

	if (!bluenoise)
	{
		// Each short edge only if its half has a row in it, which for a
		// small triangle is often only one of them. mid_is_left true means
		// the LONG edge is on the LEFT -- the name is upstream's
		// (raster_scanline_simple_begin1/2).
		const uint32_t* pat = s_pat32[t->shade <= 32 ? t->shade : 32];
		const int h1 = row_mid - row_top, h2 = row_bot - row_mid;
		uint32_t aF = 0, bF = 0;
		int32_t aS = 0, bS = 0;
		if (h1 > 0)
		{
			aS = fx_slope(mx - tx, my - ty);
			aF = fx_start(tx, ty, row_top, aS);
		}
		if (h2 > 0)
		{
			bS = fx_slope(bx - mx, by - my);
			bF = fx_start(mx, my, row_mid, bS);
		}
		const uint32_t fl = fx_start(tx, ty, row_top, sl);
		const uint32_t lF = mid_is_left ? fl : aF;
		const uint32_t rF = mid_is_left ? aF : fl;
		const int32_t lS = mid_is_left ? sl : aS;
		const int32_t rS = mid_is_left ? aS : sl;
		uint8_t* row = bitmap + row_top * rowstride;
		// All three corners on the screen across: see fx_tri_pattern_body.
		const bool inside = ((X0 | X1 | X2)
		                     | (SCREEN_X * 16 - 1 - X0)
		                     | (SCREEN_X * 16 - 1 - X1)
		                     | (SCREEN_X * 16 - 1 - X2)) >= 0;
		if (rowstride != SCREEN_STRIDE_BYTES)
			fx_tri_pattern(row, rowstride, row_top, h1, h2, lF, lS, rF, rS,
			               mid_is_left, bF, bS, pat);
		else if (inside)
			fx_tri_pattern_pd_inside(row, row_top, h1, h2, lF, lS, rF, rS,
			                         mid_is_left, bF, bS, pat);
		else
			fx_tri_pattern_pd_clip(row, row_top, h1, h2, lF, lS, rF, rS,
			                       mid_is_left, bF, bS, pat);
		return;
	}

	const uint8_t* noise = get_blue_noise_buffer();

	// The top half, then the bottom, each between the long edge and one
	// short one.
	for (int half = 0; half < 2; ++half)
	{
		const int y  = half ? row_mid : row_top;
		const int h  = half ? row_bot - row_mid : row_mid - row_top;
		if (h <= 0)
			continue;
		const int32_t ss = half ? fx_slope(bx - mx, by - my) : fx_slope(mx - tx, my - ty);
		const uint32_t fs = half ? fx_start(mx, my, y, ss) : fx_start(tx, ty, y, ss);
		// The long edge at this half's first row, from its top vertex. The
		// pattern fill carries it over from the first half instead, one add
		// a row, and it is the same number: fx_start's
		// floor((16Y - ty) * S / 16) plus n * S is
		// floor((16(Y + n) - ty) * S / 16) exactly, since 16nS is a multiple
		// of 16 -- and both sides wrap mod 2^32 alike.
		const uint32_t fl = fx_start(tx, ty, y, sl);
		uint8_t* row = bitmap + y * rowstride;
		const uint32_t lF = mid_is_left ? fl : fs;
		const uint32_t rF = mid_is_left ? fs : fl;
		const int32_t lS = mid_is_left ? sl : ss;
		const int32_t rS = mid_is_left ? ss : sl;
		if (rowstride == SCREEN_STRIDE_BYTES)
			fx_rows_bluenoise_pd(row, y, h, lF, lS, rF, rS, noise, t->shade);
		else
			fx_rows_bluenoise(row, rowstride, y, h, lF, lS, rF, rS, noise, t->shade);
	}
}

static FX_NOINLINE void fx_fill_triangle_pattern(uint8_t* bitmap, int rowstride, const SceneTri* t)
{
	fx_fill_triangle_body(bitmap, rowstride, t, false);
}

static FX_NOINLINE void fx_fill_triangle_bluenoise(uint8_t* bitmap, int rowstride, const SceneTri* t)
{
	fx_fill_triangle_body(bitmap, rowstride, t, true);
}

// The float point a 28.4 value came from, near enough for a wire line.
FORCE_INLINE float3 fx_to_point(int x, int y)
{
	return f3((float)x * (1.0f / 16.0f) + 0.5f, (float)y * (1.0f / 16.0f) + 0.5f, 0.0f);
}

static void draw_scene_tri(uint8_t* bitmap, int rowstride, const SceneTri* t,
                           enum SceneDrawFlags scene_draw_flags)
{
	if (t->flags & MeshDrawFlag_Solid)
	{
		if (scene_draw_flags & SceneDrawFlag_DitherBluenoise)
			fx_fill_triangle_bluenoise(bitmap, rowstride, t);
		else
			fx_fill_triangle_pattern(bitmap, rowstride, t);
	}

	if (t->flags & (MeshDrawFlag_Wire | MeshDrawFlag_SilhouetteWire))
	{
		const float3 p1 = fx_to_point(t->x[0], t->y[0]);
		const float3 p2 = fx_to_point(t->x[1], t->y[1]);
		const float3 p3 = fx_to_point(t->x[2], t->y[2]);
		if (t->wire_flag & 1) draw_black_line(bitmap, rowstride, &p1, &p2);
		if (t->wire_flag & 2) draw_black_line(bitmap, rowstride, &p2, &p3);
		if (t->wire_flag & 4) draw_black_line(bitmap, rowstride, &p3, &p1);
	}
}

static inline uint32_t float_flip(uint32_t f)
{
	uint32_t mask = -((int32_t)(f >> 31)) | 0x80000000;
	return f ^ mask;
}

static inline uint32_t pack_depth_index(float z, uint32_t index)
{
	union { float f; uint32_t u; } bits;
	// pack depth as FP16 into top bits (flipped negatives so that radix sort ordering works)
	bits.f = z;
	bits.u = float_to_half_topbits(z);
	bits.u = float_flip(bits.u);
	// put index into lowest 16 bits
	bits.u = (bits.u & 0xFFFF0000) | index;
	return bits.u;
}

static bool is_face_visible(const Scene* scene, uint16_t fi)
{
	return (fi != 0xFFFF) && (scene->tmp_vis_tris_mask[fi >> 5] & (1u << (fi & 31)));
}

// --------------------------------------------------------------------------
// Clipping
//
// Two things have to be bounded before a triangle reaches the rasterizer.
//
// NEAR PLANE. A triangle with every vertex in front of it goes down the fast
// path untouched; one with every vertex behind it is dropped. The remainder
// is cut against z = k_near_z in VIEW space, which leaves a triangle or a
// quad. Upstream drops these whole, which is invisible in a scene viewed from
// outside and takes the roof off when you are standing inside a tunnel.
//
// GUARD BAND. tri_edge_init sets up its DDA in 28.4 fixed point and forms
// products of order dN * topXfx -- about 1024 * C^2 for a projected
// coordinate of magnitude C. That overflows int32 above C of roughly 1400,
// and the result is a garbage edge slope: a triangle that rasterizes as a
// wrong, usually enormous, filled shape.
//
// C gets large because projection divides by view z, and near the near plane
// that divisor is tiny: a vertex at z = k_near_z one unit off axis projects
// to 16000 pixels. Measured over a lap of Virtua Racing, unclipped geometry
// alone reaches 435000 pixels and clipped geometry reaches 1000000, so this
// is NOT only a consequence of near clipping -- upstream has it too, for any
// vertex that sits just in front of the near plane. Near clipping makes it
// far more common by placing vertices exactly there.
//
// So the projected polygon is also clipped to a band around the screen. That
// is exact rather than approximate: the polygon really is cut, not clamped,
// and this renderer interpolates nothing across a triangle -- the fill is a
// flat brightness and both dither modes are indexed by screen position -- so
// a 2D clip after the perspective divide changes no pixel inside the band.
//
// The clipped vertices have no vertex index of their own, so they go in a
// scratch list and the sort refers to them as mesh->tri_count + n;
// tmp_clip_src maps back to the source triangle for its colour, its adjacency
// and its visibility bit. The lighting normal is taken from the source
// triangle too: a piece is coplanar with its parent, so the parent's normal
// is exactly right and costs nothing.
// --------------------------------------------------------------------------

// The near plane, in world units. Upstream's 0.015 is not a considered
// value -- it is an epsilon to keep the perspective divide finite, from a
// renderer that never clips and only needs "not zero". Once triangles are
// CLIPPED to this plane rather than dropped at it, it becomes a real near
// plane and the number matters:
//
//   * A vertex clipped to z = 0.015 projects at 240/0.015 = 16000 pixels per
//     world unit off axis, so a clipped ground polygon becomes enormous and
//     is drawn in full. Over a lap of Acropolis the worst frame cost 256k
//     fill pixels, nearly three screenfuls, against 174k unclipped.
//   * At 0.25 the same worst frame costs 165k -- BELOW the unclipped build,
//     which pays its own price for the rasterizer overflow the guard band
//     now prevents. The median cost of clipping drops to about 6%.
//
// 0.25 world units is 64 ROM units, and it costs nothing visible: the eye
// rides 1.1 units above the road, so the plane cuts the ground at a screen
// row far below the bottom of the display. What it does cut is geometry the
// camera is almost touching, which is what a near plane is for.
static const float k_near_z = 0.25f;   // must match k_min_z below

// Half the margin the guard band adds around the screen. The bound above
// wants max |coordinate| under about 1400; SCREEN_X + 512 is 912, which
// leaves the products an order of magnitude of headroom. Making it smaller
// only means clipping more often -- it cannot change what is drawn.
#define CLIP_GUARD 512.0f
#define CLIP_X0 (-CLIP_GUARD)
#define CLIP_X1 ((float)SCREEN_X + CLIP_GUARD)
#define CLIP_Y0 (-CLIP_GUARD)
#define CLIP_Y1 ((float)SCREEN_Y + CLIP_GUARD)

// OUTCODES. One byte per vertex, worked out where the vertex is projected,
// saying which side of each screen edge and of the guard band it is on. The
// cull loop then rejects on the AND of three bytes and asks for guard-band
// clipping on their OR, without reloading and comparing nine floats a
// triangle. A vertex behind the near plane has no projection, so it carries
// VC_NEAR and nothing else.
//
// The screen bits are taken from the vertex's 28.4 point, the one the
// rasterizer will fill from, and they say "every pixel centre is on the far
// side of this edge": x < 0 in 28.4-with-offset is a vertex left of every
// column's centre, x >= 400 * 16 one right of all of them. So a triangle with
// all three corners off one edge covers no pixel, which is the only thing the
// reject needs to be true -- and it is exactly true, where the float test it
// replaced was half a pixel out in each direction. A triangle it disagreed
// about drew nothing either way.
enum
{
	VC_LEFT   = 1 << 0,
	VC_RIGHT  = 1 << 1,
	VC_TOP    = 1 << 2,
	VC_BOTTOM = 1 << 3,
	VC_SCREEN = VC_LEFT | VC_RIGHT | VC_TOP | VC_BOTTOM,
	VC_GUARD  = 1 << 4,   // outside the guard band on some axis
	VC_NEAR   = 1 << 5,   // z < k_near_z: no projection
};

// The outcode and the 28.4 point of a projected vertex, in one go. `xf`
// and `yf` are the screen point already in 28.4 with the half-pixel offset
// taken off -- (x - 0.5) * 16 -- still as floats.
//
// Outside the guard band the point is not converted at all -- it could be
// anywhere, and a float out of an int's range is undefined -- and the
// triangle goes to the float clipping path, which never reads it. The screen
// bits there come from the floats, by the same rule.
#define GUARD16_X0 ((CLIP_X0 - 0.5f) * 16.0f)
#define GUARD16_X1 ((CLIP_X1 - 0.5f) * 16.0f)
#define GUARD16_Y0 ((CLIP_Y0 - 0.5f) * 16.0f)
#define GUARD16_Y1 ((CLIP_Y1 - 0.5f) * 16.0f)

FORCE_INLINE uint8_t vert_fix(float xf, float yf, uint32_t* fix)
{
	if (fabsf(xf - 0.5f * (GUARD16_X0 + GUARD16_X1)) > 0.5f * (GUARD16_X1 - GUARD16_X0) ||
	    fabsf(yf - 0.5f * (GUARD16_Y0 + GUARD16_Y1)) > 0.5f * (GUARD16_Y1 - GUARD16_Y0))
	{
		*fix = 0;
		return (uint8_t)(VC_GUARD
		               | (xf < 0.0f)
		               | ((xf >= (float)(SCREEN_X * 16)) << 1)
		               | ((yf < 0.0f) << 2)
		               | ((yf >= (float)(SCREEN_Y * 16)) << 3));
	}
	const int32_t xi = (int32_t)xf;
	const int32_t yi = (int32_t)yf;
	*fix = (uint32_t)(uint16_t)xi | ((uint32_t)yi << 16);
	return (uint8_t)((xi < 0)
	               | ((xi >= SCREEN_X * 16) << 1)
	               | ((yi < 0) << 2)
	               | ((yi >= SCREEN_Y * 16) << 3));
}

// A view-space point on the screen, for the paths that still want floats:
// the guard-band clipper, a few triangles a frame.
FORCE_INLINE float3 project_pt(float3 v, float scale, float add_x, float add_y)
{
	const float inv_z = scale / v.z;
	return f3(v.x * inv_z + add_x, v.y * inv_z + add_y, v.z);
}

// One packed vertex into view space: its lattice coordinates through the
// transform that has the dequantisation folded in (see the vertex loop). Its
// own function, written exactly as the loop body it came from, because under
// -ffast-math GCC is free to re-associate the transform's sum and the order it
// picks follows how the statements are laid out: kept this way, every target
// sums the terms in the order it always has.
FORCE_INLINE float3 vtx_packed(const xform* mq, uint32_t v_bits)
{
	const float3 q = { (float)(v_bits & 2047), (float)((v_bits >> 11) & 1023), (float)(v_bits >> 21) };
	return xform_transform_pt(mq, q);
}

// A vertex's view-space point, put back together from the two arrays the
// transform pass keeps (see tmp_view_z in render.h).
FORCE_INLINE float3 view_point(const Scene* scene, int i)
{
	return f3(scene->tmp_view_xy[i].x, scene->tmp_view_xy[i].y, scene->tmp_view_z[i]);
}

FORCE_INLINE int32_t fix_x(uint32_t f) { return (int16_t)(f & 0xFFFFu); }
FORCE_INLINE int32_t fix_y(uint32_t f) { return (int32_t)f >> 16; }

// The depth a triangle sorts on. Normally its own; for a decal, the depth of
// the surface it is painted on, so the two can never cross.
//
// A coplanar decal and its host are ordered by an offset of a few hundredths
// of a unit, and that only decides while their centroids are at the same
// depth. The host is large, so turning the camera swings its centroid several
// units while the decal's barely moves; the host's key overtakes and the
// decal is painted over. Borrowing the key makes them tie at every angle, and
// the stable radix sort plus the exporter's host-then-decal ordering keeps
// the decal on top. See group_decals in tools/vr_track_export.py.
FORCE_INLINE float sort_depth_of(const float* vz, float sort_bias,
                                 const uint16_t* tris, const uint16_t* proxy, int tri,
                                 uint16_t idx0, uint16_t idx1, uint16_t idx2)
{
	if (proxy)
	{
		const uint16_t src = proxy[tri];
		if (src != tri)
		{
			const uint16_t* pb = tris + (size_t)src * 3;
			idx0 = pb[0]; idx1 = pb[1]; idx2 = pb[2];
		}
	}
	// sort_bias is the instance's sort_bias, tripled, because this is a
	// sum of three depths rather than one. Upstream added sort_bias to the
	// instance's centre depth in scene_draw; with instances no longer the
	// unit of ordering it has to reach the triangles instead, and this is the
	// one place every path -- fast, guard band, near clip -- goes through.
	return sort_bias - (vz[idx0] + vz[idx1] + vz[idx2]);
}

static inline float sort_depth(const Scene* scene, const Mesh* mesh, int tri,
                               uint16_t idx0, uint16_t idx1, uint16_t idx2)
{
	return sort_depth_of(scene->tmp_view_z, scene->tmp_sort_bias, mesh->tris,
	                     mesh->sort_proxy, tri, idx0, idx1, idx2);
}

static int clip_scratch_reserve(Scene* scene, int want)
{
	if (want <= scene->clip_cap)
		return 1;
	int cap = scene->clip_cap ? scene->clip_cap : 64;
	while (cap < want)
		cap *= 2;
	float3* pts = plat_realloc(scene->tmp_clip_pts, (size_t)cap * 3 * sizeof(float3));
	if (!pts)
		return 0;
	scene->tmp_clip_pts = pts;
	uint16_t* src = plat_realloc(scene->tmp_clip_src, (size_t)cap * sizeof(uint16_t));
	if (!src)
		return 0;   // pts kept; clip_cap not bumped, so the space is just unused
	scene->tmp_clip_src = src;
	scene->clip_cap = cap;
	return 1;
}

// Sutherland-Hodgman against one axis-aligned half-plane in 2D.
// side: 0 = x >= limit, 1 = x <= limit, 2 = y >= limit, 3 = y <= limit.
static int clip_poly_axis(const float3* in, int n, float3* out, int side, float limit)
{
	int m = 0;
	for (int i = 0; i < n; ++i)
	{
		const float3 a = in[i];
		const float3 b = in[(i + 1) % n];
		const float av = (side < 2) ? a.x : a.y;
		const float bv = (side < 2) ? b.x : b.y;
		const bool a_in = (side == 0 || side == 2) ? (av >= limit) : (av <= limit);
		const bool b_in = (side == 0 || side == 2) ? (bv >= limit) : (bv <= limit);
		if (a_in)
			out[m++] = a;
		if (a_in != b_in)
		{
			// One endpoint each side, so bv - av cannot be zero.
			const float t = (limit - av) / (bv - av);
			out[m].x = a.x + (b.x - a.x) * t;
			out[m].y = a.y + (b.y - a.y) * t;
			out[m].z = a.z + (b.z - a.z) * t;
			++m;
		}
	}
	return m;
}

// Cuts `poly` (n projected points, in mesh winding order) to the guard band,
// fans the result, and appends whatever survives to the order table.
// Returns how many entries were appended.
static int emit_clipped_poly(Scene* scene, const Mesh* mesh, uint16_t tri,
                             const float3* poly, int n, float sort_key,
                             bool use_silhouette, int vis_tri_count)
{
	// Two buffers, ping-ponged through the four half-planes. Four vertices in,
	// at most one new vertex per plane, so eight is the ceiling.
	float3 buf_a[8], buf_b[8];
	const float3* cur = poly;

	float minx = poly[0].x, maxx = poly[0].x;
	float miny = poly[0].y, maxy = poly[0].y;
	for (int i = 1; i < n; ++i)
	{
		minx = min2f(minx, poly[i].x); maxx = max2f(maxx, poly[i].x);
		miny = min2f(miny, poly[i].y); maxy = max2f(maxy, poly[i].y);
	}

	// Entirely off screen: nothing to draw, and no need to clip it first.
	if (maxx < 0.0f || minx >= (float)SCREEN_X ||
		maxy < 0.0f || miny >= (float)SCREEN_Y)
		return 0;

	if (minx < CLIP_X0 || maxx > CLIP_X1 || miny < CLIP_Y0 || maxy > CLIP_Y1)
	{
		n = clip_poly_axis(cur, n, buf_a, 0, CLIP_X0); cur = buf_a; if (n < 3) return 0;
		n = clip_poly_axis(cur, n, buf_b, 1, CLIP_X1); cur = buf_b; if (n < 3) return 0;
		n = clip_poly_axis(cur, n, buf_a, 2, CLIP_Y0); cur = buf_a; if (n < 3) return 0;
		n = clip_poly_axis(cur, n, buf_b, 3, CLIP_Y1); cur = buf_b; if (n < 3) return 0;
	}

	int tri_count = n - 2;
	// The order table is indexed straight, so this is the only thing standing
	// between a mis-sized allocation and a write past the end of it.
	if (vis_tri_count + tri_count > scene->order_cap)
		tri_count = scene->order_cap - vis_tri_count;
	if (tri_count <= 0)
		return 0;
	if (!clip_scratch_reserve(scene, scene->clip_count + tri_count))
		return 0;

	int emitted = 0;
	for (int t = 0; t < tri_count; ++t)
	{
		const float3 q0 = cur[0];
		const float3 q1 = cur[t + 1];
		const float3 q2 = cur[t + 2];

		// Same winding rule as the fast path -- clipping preserves orientation.
		const float d = (q1.x - q0.x) * (q2.y - q0.y) - (q1.y - q0.y) * (q2.x - q0.x);
		if (d <= 0)
			continue;

		const int c = scene->clip_count;
		scene->tmp_clip_pts[c * 3 + 0] = q0;
		scene->tmp_clip_pts[c * 3 + 1] = q1;
		scene->tmp_clip_pts[c * 3 + 2] = q2;
		scene->tmp_clip_src[c] = tri;
		scene->clip_count = c + 1;

		// sort_key comes from the SOURCE triangle, not from q0/q1/q2.
		//
		// A clipped piece has vertices sitting exactly on the near plane, so
		// its own depth midpoint is the smallest one possible, and it would
		// sort in front of everything. A ground triangle running fifty units
		// into the distance would be drawn last purely because it happens to
		// pass under the camera, covering every object standing on it.
		//
		// That is what "big polygons crossing the screen" looked like after
		// near clipping was added, and it cost the fill to match: the worst
		// frame of a lap of Acropolis rose from 174k pixels to 256k, nearly
		// three screenfuls, because a near-plane polygon was being painted
		// over the whole scene.
		//
		// Keying on the parent keeps a piece exactly where the triangle it
		// came from would have sorted, so clipping changes what is drawn
		// without changing the order it is drawn in.
		scene->tmp_order_table1[vis_tri_count + emitted] =
			pack_depth_index(sort_key, (uint32_t)(mesh->tri_count + c));
		++emitted;
	}

	if (emitted && use_silhouette)
		scene->tmp_vis_tris_mask[tri >> 5] |= 1u << (tri & 31);

	return emitted;
}

// Straddles the near plane: cut in view space, project, then hand to the
// guard-band path above.
static int clip_triangle_near(Scene* scene, const Mesh* mesh, uint16_t tri,
                              uint16_t idx0, uint16_t idx1, uint16_t idx2,
                              float scene_scale, float proj_add_x, float proj_add_y,
                              bool use_silhouette, int vis_tri_count)
{
	const float3 in[3] = {
		view_point(scene, idx0),
		view_point(scene, idx1),
		view_point(scene, idx2),
	};
	float3 poly[4];
	int n = 0;
	for (int e = 0; e < 3; ++e)
	{
		const float3 a = in[e];
		const float3 b = in[(e + 1) % 3];
		const bool a_in = a.z >= k_near_z;
		const bool b_in = b.z >= k_near_z;
		if (a_in)
			poly[n++] = a;
		if (a_in != b_in)
		{
			const float t = (k_near_z - a.z) / (b.z - a.z);
			poly[n].x = a.x + (b.x - a.x) * t;
			poly[n].y = a.y + (b.y - a.y) * t;
			poly[n].z = k_near_z;
			++n;
		}
	}
	if (n < 3)
		return 0;

	float3 pp[4];
	for (int k = 0; k < n; ++k)
	{
		const float inv_z = scene_scale / poly[k].z;
		pp[k].x = poly[k].x * inv_z + proj_add_x;
		pp[k].y = poly[k].y * inv_z + proj_add_y;
		pp[k].z = poly[k].z;
	}

	// Depth from the source triangle, with each vertex pulled up to the near
	// plane first: the part behind the camera has no depth worth averaging,
	// and a negative z would pack into a nonsense key.
	float key;
	if (mesh->sort_proxy && mesh->sort_proxy[tri] != tri)
	{
		// Borrowed key: take the host's, clamped the same way.
		const uint16_t* pb = mesh->tris + (size_t)mesh->sort_proxy[tri] * 3;
		key = -(max2f(scene->tmp_view_z[pb[0]], k_near_z)
		      + max2f(scene->tmp_view_z[pb[1]], k_near_z)
		      + max2f(scene->tmp_view_z[pb[2]], k_near_z));
	}
	else
	{
		const float z0 = max2f(in[0].z, k_near_z);
		const float z1 = max2f(in[1].z, k_near_z);
		const float z2 = max2f(in[2].z, k_near_z);
		key = -(z0 + z1 + z2);
	}

	return emit_clipped_poly(scene, mesh, tri, pp, n, key,
	                         use_silhouette, vis_tri_count);
}

// ONE NORMAL PER FACE, IN THE MESH'S OWN SPACE, built once.
//
// The lighting used to take each visible triangle's normal from its three
// view-space points every frame: two subtractions, a cross product and a
// normalise -- a square root and a divide, each about fourteen cycles on the
// M7's FPU. The mesh does not change shape, so the normal only changes by the
// instance's rotation, and the light can be turned the other way instead,
// once per instance (light_in_model below). Same number, one dot product.
//
// Computed with the same function on the same dequantised points the vertex
// loop sees, so the result differs from the old one only in float rounding.
// Meshes with several vertex frames change shape and keep the old path.
static const float3* mesh_face_normals(Mesh* mesh)
{
	if (mesh->face_normals)
		return mesh->face_normals;
	if (mesh->vertex_frame_count != 1 || mesh->tri_count <= 0)
		return NULL;
	float3* n = plat_malloc((size_t)mesh->tri_count * sizeof(float3));
	float3* v = plat_malloc((size_t)mesh->vertex_count * sizeof(float3));
	if (!n || !v)
	{
		plat_free(n);
		plat_free(v);
		return NULL;
	}
	if ((mesh->flags & MeshFlag_FloatVertices) == 0)
	{
		const uint32_t* src = (const uint32_t*)mesh->vertices;
		const float3 bbox_scale = v3_mul(v3_sub(mesh->bbox_max, mesh->bbox_min), f3(1.0f / 2047.0f, 1.0f / 1023.0f, 1.0f / 2047.0f));
		for (int i = 0; i < mesh->vertex_count; ++i)
		{
			const uint32_t b = src[i];
			const float3 q = { (float)(b & 2047), (float)((b >> 11) & 1023), (float)(b >> 21) };
			v[i] = v3_add(v3_mul(q, bbox_scale), mesh->bbox_min);
		}
	}
	else
	{
		memcpy(v, mesh->vertices, (size_t)mesh->vertex_count * sizeof(float3));
	}
	for (int t = 0; t < mesh->tri_count; ++t)
	{
		const uint16_t* ib = mesh->tris + t * 3;
		// The same argument order the view-space version used: 0, 2, 1.
		n[t] = v3_tri_normal(&v[ib[0]], &v[ib[2]], &v[ib[1]]);
	}
	plat_free(v);
	mesh->face_normals = n;
	return n;
}

// The view-space light, in the space of a mesh drawn with model-view `mv`.
//
// With view points p' = M p + t, a face's view normal is
// normalize(cross(M a, M b)) = sign(det M) * R n for M = s R, R orthonormal
// -- a uniform scale s and a rotation, possibly mirrored, which is every
// transform this renderer is given (the view flips y). So
// dot(n_view, L) = dot(n, sign(det M) * R^T L), and R^T L is M^T L
// normalised. Nine multiplies and a normalise, once per instance.
static float3 light_in_model(const xform* mv, float3 light)
{
	const float3 l = f3(v3_dot(mv->ax, light), v3_dot(mv->ay, light), v3_dot(mv->az, light));
	const float det = v3_dot(mv->ax, v3_cross(mv->ay, mv->az));
	const float3 n = v3_normalize(l);
	return det < 0.0f ? v3_neg(n) : n;
}

// The brightness into what the fill indexes by: a dither pattern, or a
// blue-noise threshold. draw_triangle made the same two conversions at draw
// time.
FORCE_INLINE uint8_t shade_of(float brightness, bool bluenoise)
{
	return bluenoise
		? (uint8_t)clamp_i((int)(brightness * 255.0f), 0, 255)
		: (uint8_t)clamp_i((int)(brightness * 32.99f), 0, 32);
}

// EACH FACE'S SHADE, KEPT WHILE NOTHING IT DEPENDS ON CHANGES.
//
// A face's shade is its colour, the instance's colour, and -- lit -- its
// normal against the light in the mesh's own space. For a piece of course
// all of those are fixed: the mesh does not move and the sun does not either,
// so the light in model space is the same vector every frame, give or take
// float noise from going to view space and back. Relighting every visible
// face every frame worked out the same byte again and again.
//
// So the byte is kept. It is trusted only while the light, the colour and
// the fill are what it was built for; and it is only BUILT once a mesh has
// been drawn twice running under the same light, because building covers
// every face and not just the visible ones -- which a mesh that turns every
// frame, like the car, would pay for and never get back. That mesh keeps the
// per-visible-face path it always had. An unlit mesh has no light to agree
// on and is cached on its first draw.
//
// "The same light" is within 1e-4 per component. The frame-to-frame noise
// is around 1e-7; the car turning by a hundredth of a degree is more than
// the tolerance.
#define SHADE_LIGHT_TOL 1e-4f

FORCE_INLINE bool light_close(float3 a, float3 b)
{
	return fabsf(a.x - b.x) < SHADE_LIGHT_TOL
	    && fabsf(a.y - b.y) < SHADE_LIGHT_TOL
	    && fabsf(a.z - b.z) < SHADE_LIGHT_TOL;
}

static const uint8_t* face_shades(Mesh* mesh, const MeshInstance* inst,
                                  const float3* normals, float3 light_m,
                                  bool use_lighting, bool bluenoise)
{
	// Several vertex frames: the normals come from this frame's points.
	if (use_lighting && normals == NULL)
		return NULL;

	const uint8_t mode = (uint8_t)((use_lighting ? 1 : 0) | (bluenoise ? 2 : 0));
	const bool same_as_last = light_close(light_m, mesh->shade_seen);
	mesh->shade_seen = light_m;

	if (mesh->face_shade != NULL && mesh->shade_mode == mode
	    && mesh->shade_color == inst->color
	    && (!use_lighting || light_close(light_m, mesh->shade_light)))
		return mesh->face_shade;

	if (use_lighting && !same_as_last)
		return NULL;      // turning relative to the light: not worth keeping

	if (mesh->face_shade == NULL)
	{
		mesh->face_shade = plat_malloc((size_t)mesh->tri_count);
		if (mesh->face_shade == NULL)
			return NULL;
	}
	for (int f = 0; f < mesh->tri_count; ++f)
	{
		float brightness = inst->color;
		if (use_lighting)
			brightness *= v3_dot(normals[f], light_m) * 0.5f + 0.5f;
		if (mesh->colors)
			brightness *= mesh->colors[f] * (1.0f / 255.0f);
		mesh->face_shade[f] = shade_of(brightness, bluenoise);
	}
	mesh->shade_light = light_m;
	mesh->shade_color = inst->color;
	mesh->shade_mode  = mode;
	return mesh->face_shade;
}

// A queued triangle's three corners, from three packed 28.4 points (x in the
// low half, y in the high, as tmp_fix_points holds them). The six int16s are
// x0 x1 x2 y0 y1 y2 in memory, which on a little-endian machine is three words
// that each take two halves of the packed points as they stand -- a PKHBT or
// PKHTB apiece on the M7. Written field by field, GCC split the three points
// into six registers, kept them alive from the facing test to here, and
// spilled them in the cull loop.
_Static_assert(offsetof(SceneTri, x) == 0 && offsetof(SceneTri, y) == 6,
               "scene_tri_put_points writes x[3], y[3] as three words");

FORCE_INLINE void scene_tri_put_points(SceneTri* out, uint32_t a, uint32_t b, uint32_t c)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	out->x[0] = (int16_t)fix_x(a); out->y[0] = (int16_t)fix_y(a);
	out->x[1] = (int16_t)fix_x(b); out->y[1] = (int16_t)fix_y(b);
	out->x[2] = (int16_t)fix_x(c); out->y[2] = (int16_t)fix_y(c);
#else
	const uint32_t w0 = (a & 0xFFFFu) | (b << 16);           // x0 x1
	const uint32_t w1 = (c & 0xFFFFu) | (a & 0xFFFF0000u);   // x2 y0
	const uint32_t w2 = (b >> 16)     | (c & 0xFFFF0000u);   // y1 y2
	uint8_t* const o = (uint8_t*)out;
	memcpy(o + 0, &w0, 4);
	memcpy(o + 4, &w1, 4);
	memcpy(o + 8, &w2, 4);
#endif
}

// And the rest of the entry, the same way: one word for the shade, the wire
// flags and the instance's flags.
_Static_assert(offsetof(SceneTri, shade) == 12 && offsetof(SceneTri, wire_flag) == 13 &&
               offsetof(SceneTri, flags) == 14 && sizeof(SceneTri) == 16,
               "scene_tri_put_rest writes bytes 12..15 as one word");

FORCE_INLINE void scene_tri_put_rest(SceneTri* out, uint8_t shade, uint8_t wire_flag, uint16_t flags)
{
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	out->shade = shade;
	out->wire_flag = wire_flag;
	out->flags = flags;
#else
	const uint32_t w3 = (uint32_t)shade | ((uint32_t)wire_flag << 8) | ((uint32_t)flags << 16);
	memcpy((uint8_t*)out + 12, &w3, 4);
#endif
}

// A face's shade the long way: its colour, the instance's, and -- lit -- its
// normal against the light. What face_shades caches for a mesh that holds
// still; this is for one that does not, like the car.
FORCE_INLINE uint8_t face_shade_value(const Scene* scene, const Mesh* mesh,
                                      const MeshInstance* inst, const float3* normals,
                                      float3 light_m, bool use_lighting, bool bluenoise,
                                      int fi, uint16_t idx0, uint16_t idx1, uint16_t idx2)
{
	float brightness = inst->color;
	if (normals)
		brightness *= v3_dot(normals[fi], light_m) * 0.5f + 0.5f;   // wrapped diffuse
	else if (use_lighting)
	{
		// A mesh with several vertex frames has no stored normals: its
		// shape changes with the frame. Taken from the view-space points.
		const float3 v0 = view_point(scene, idx0);
		const float3 v1 = view_point(scene, idx1);
		const float3 v2 = view_point(scene, idx2);
		float3 normal = v3_tri_normal(&v0, &v2, &v1);
		brightness *= v3_dot(normal, scene->light) * 0.5f + 0.5f;
	}
	if (mesh->colors)
		brightness *= mesh->colors[fi] * (1.0f / 255.0f);
	return shade_of(brightness, bluenoise);
}

// Queues the triangles whose keys are order[from, to) -- the instance's own,
// or pieces the clippers cut from them -- resolved to what the rasteriser
// needs. The cull loop queues an ordinary triangle itself; this is what it
// calls for clipped pieces, and what the silhouette style, which needs the
// whole mesh culled before it can tell which edges are silhouettes, runs over
// everything afterwards.
static void resolve_range(Scene* scene, const Mesh* mesh, const MeshInstance* inst,
                          const uint8_t* shades, const float3* normals, float3 light_m,
                          bool use_lighting, bool bluenoise, const uint32_t* fixp,
                          const uint32_t* order, int from, int to)
{
	const int tri_count = mesh->tri_count;
	const uint16_t* const tris = mesh->tris;
	int queued = scene->scene_tri_count;
	SceneTri* const queue = scene->tmp_scene_tris;
	uint32_t* const queue_keys = scene->tmp_scene_order1;
	const uint16_t inst_flags = (uint16_t)inst->flags;
	for (int i = from; i < to; ++i)
	{
		// The index half of a key is 16 bits; see the same test in the cull
		// loop.
		if (queued >= 0xFFFF)
			break;
		const uint32_t key = order[i];
		uint16_t fi = key & 0xFFFF;

		// An index past the end of the mesh is a piece of a triangle that was
		// clipped against the near plane: its geometry is in the clip scratch,
		// everything else still comes from the triangle it was cut from.
		const bool clipped = (fi >= tri_count);
		const float3* geom = NULL;
		if (clipped)
		{
			const int c = fi - tri_count;
			geom = &scene->tmp_clip_pts[c * 3];
			fi = scene->tmp_clip_src[c];
		}

		uint16_t idx0 = tris[fi * 3 + 0];
		uint16_t idx1 = tris[fi * 3 + 1];
		uint16_t idx2 = tris[fi * 3 + 2];

		const uint8_t shade = shades ? shades[fi]
		                    : face_shade_value(scene, mesh, inst, normals, light_m,
		                                       use_lighting, bluenoise, fi, idx0, idx1, idx2);

		uint8_t wire_flag = 0xFF;
		if (inst->flags & MeshDrawFlag_SilhouetteWire)
		{
			const uint16_t opf0 = mesh->adjacency[fi * 3 + 0];
			const uint16_t opf1 = mesh->adjacency[fi * 3 + 1];
			const uint16_t opf2 = mesh->adjacency[fi * 3 + 2];
			if (is_face_visible(scene, opf0))
				wire_flag &= ~4;
			if (is_face_visible(scene, opf1))
				wire_flag &= ~2;
			if (is_face_visible(scene, opf2))
				wire_flag &= ~1;
		}
		else if (inst->flags & MeshDrawFlag_Wire)
		{
			if (mesh->wire_flags)
				wire_flag = mesh->wire_flags[fi];
		}
		// The unclipped case swaps 1 and 2; the clipped one does not, because
		// clip_triangle_near already emitted its vertices in mesh order and
		// tested them with the same rule the fast path uses. The swap is done
		// HERE rather than at the draw call, so a queued entry is three points
		// and needs no further interpretation.
		const int g = queued++;
		SceneTri* out = &queue[g];
		if (clipped)
		{
			// Into the rasterizer's own format: 28.4 fixed point with the
			// half-pixel centre offset taken off, exactly the conversion
			// tri_edge_init makes (FloatToFixed28_4(v - 0.5f)).
			const float3* q0 = &geom[0];
			const float3* q1 = &geom[2];
			const float3* q2 = &geom[1];
			out->x[0] = (int16_t)(int32_t)((q0->x - 0.5f) * 16.0f);
			out->y[0] = (int16_t)(int32_t)((q0->y - 0.5f) * 16.0f);
			out->x[1] = (int16_t)(int32_t)((q1->x - 0.5f) * 16.0f);
			out->y[1] = (int16_t)(int32_t)((q1->y - 0.5f) * 16.0f);
			out->x[2] = (int16_t)(int32_t)((q2->x - 0.5f) * 16.0f);
			out->y[2] = (int16_t)(int32_t)((q2->y - 0.5f) * 16.0f);
		}
		else
		{
			// Already converted, once per vertex, by vert_fix.
			scene_tri_put_points(out, fixp[idx0], fixp[idx2], fixp[idx1]);
		}
		scene_tri_put_rest(out, shade, wire_flag, inst_flags);
		// Keep the depth half of the key the cull loop computed and point the
		// index half at this entry. The depth is already flipped into
		// sortable form by pack_depth_index, so nothing is recomputed and
		// nothing is lost.
		queue_keys[g] = (key & 0xFFFF0000u) | (uint32_t)g;
	}
	scene->scene_tri_count = queued;
}

// Transform, cull, clip and shade one instance, and queue what survives for
// the scene-wide sort in scene_draw. This used to draw as well -- hence the
// name it had, draw_mesh_instance -- and the split is the whole of the fix
// described at SceneTri in render.h.
static void collect_mesh_instance(Scene* scene, const MeshInstance* inst)
{
	Mesh* mesh = inst->mesh;
	if (!mesh)
		return;
	const xform* matrix = &inst->tr;
	const bool use_silhouette = inst->flags & MeshDrawFlag_SilhouetteWire;

	// Conservative reject against the view frustum, done BEFORE anything
	// else: without it every vertex of an off-screen instance is still
	// transformed and projected.
	//
	// Against the mesh's box rather than a sphere round it. A course segment
	// is a long flat strip, and the sphere that holds one is as tall as the
	// segment is long: where the course folds back on itself -- Acropolis'
	// hairpin -- several segments a frame were inside the sphere test and
	// outside the screen, and paid the whole vertex pass for no triangles.
	// The box's extent along a plane's normal is the sum of its three
	// half-edges' extents along it, so this is exact for the box (nothing
	// with a corner on screen is ever dropped) and costs no more than the
	// sphere did.
	{
		const xform* v = &scene->view_xform;
		const float3 c = xform_transform_pt(v, xform_transform_pt(matrix, mesh->center));
		const float3 e = v3_mulfl(v3_sub(mesh->bbox_max, mesh->bbox_min), 0.5f);
#define VIEW_DIR(a) f3((a).x * v->ax.x + (a).y * v->ay.x + (a).z * v->az.x, \
                       (a).x * v->ax.y + (a).y * v->ay.y + (a).z * v->az.y, \
                       (a).x * v->ax.z + (a).y * v->ay.z + (a).z * v->az.z)
		const float3 hx = v3_mulfl(VIEW_DIR(matrix->ax), e.x);
		const float3 hy = v3_mulfl(VIEW_DIR(matrix->ay), e.y);
		const float3 hz = v3_mulfl(VIEW_DIR(matrix->az), e.z);
#undef VIEW_DIR

		// entirely nearer than the near plane (matches k_min_z below)
		if (c.z + fabsf(hx.z) + fabsf(hy.z) + fabsf(hz.z) < 0.015f)
			return;

		const float S = scene->scale;
		const float px = 1.6666666f * scene->centerx * S;
		const float py = scene->centery * S;
		// Each screen edge is a plane through the eye, n . p = 0 with the
		// inside positive. The box is outside when its centre is further out
		// than the box reaches along n.
#define OUTSIDE(nx, nz, comp) \
		((nx) * c.comp + (nz) * c.z \
		 < -(fabsf((nx) * hx.comp + (nz) * hx.z) + fabsf((nx) * hy.comp + (nz) * hy.z) \
		     + fabsf((nx) * hz.comp + (nz) * hz.z)))
		if (OUTSIDE(S, px, x)) return;                            // left:   screen_x >= 0
		if (OUTSIDE(-S, (float)SCREEN_X - px, x)) return;         // right:  screen_x < SCREEN_X
		if (OUTSIDE(S, py, y)) return;                            // top
		if (OUTSIDE(-S, (float)SCREEN_Y - py, y)) return;         // bottom
#undef OUTSIDE
	}

	scene->tmp_sort_bias = -3.0f * inst->sort_bias;

	// temporary buffers
	if (scene->tmp_points_cap < mesh->vertex_count) {
		scene->tmp_points_cap = mesh->vertex_count;
		scene->tmp_view_z = plat_realloc(scene->tmp_view_z, scene->tmp_points_cap * sizeof(scene->tmp_view_z[0]));
		scene->tmp_view_xy = plat_realloc(scene->tmp_view_xy, scene->tmp_points_cap * sizeof(scene->tmp_view_xy[0]));
		scene->tmp_vert_codes = plat_realloc(scene->tmp_vert_codes, scene->tmp_points_cap * sizeof(scene->tmp_vert_codes[0]));
		scene->tmp_fix_points = plat_realloc(scene->tmp_fix_points, scene->tmp_points_cap * sizeof(scene->tmp_fix_points[0]));
	}
	// Six entries per triangle, not one. A triangle can fan into that many:
	// the near plane cuts it to a quad, the guard band's four half-planes add
	// at most one vertex each, so eight, and an eight-sided polygon fans into
	// six. Four is the most a lap of any of these courses actually reaches --
	// but the table is indexed without a bound check in the loop below, so
	// the allocation is the bound, and it is sized for the worst case rather
	// than the measured one. emit_clipped_poly stops at order_cap as well.
	if (scene->order_cap < mesh->tri_count * 6) {
		scene->order_cap = mesh->tri_count * 6;
		plat_free(scene->tmp_order_table1);
		plat_free(scene->tmp_order_table2);
		scene->tmp_order_table1 = plat_malloc(scene->order_cap * sizeof(scene->tmp_order_table1[0]));
		scene->tmp_order_table2 = plat_malloc(scene->order_cap * sizeof(scene->tmp_order_table2[0]));
	}
	if (use_silhouette) {
		// initially mark all triangles as not visible if we're rendering with silhouette edges
		int mesh_tri_mask_count = ((mesh->tri_count * 3) + 31) >> 5;
		if (scene->vis_tris_mask_cap < mesh_tri_mask_count) {
			scene->vis_tris_mask_cap = mesh_tri_mask_count;
			plat_free(scene->tmp_vis_tris_mask);
			scene->tmp_vis_tris_mask = plat_malloc(scene->vis_tris_mask_cap * sizeof(scene->tmp_vis_tris_mask[0]));
		}
		memset(scene->tmp_vis_tris_mask, 0, mesh_tri_mask_count * sizeof(scene->tmp_vis_tris_mask[0]));

		// ensure mesh has adjancency calculated
		if (!mesh->adjacency)
		{
			mesh->adjacency = plat_malloc(mesh->tri_count * 3 * sizeof(uint16_t));
			generate_adjacency_buffer(mesh->adjacency, mesh->tris, mesh->tri_count * 3);
		}
	}

	// Transform AND project, one pass per vertex.
	//
	// Model and view are folded into one transform per instance, and the
	// packed format's dequantisation (q * scale + bbox_min) is folded into
	// that too: scaling the three axes by the quantisation steps and moving
	// the origin to bbox_min's image makes the integer lattice coordinates the
	// transform's input directly. Per vertex that is three int-to-float
	// conversions and one matrix-vector product, then the divide.
	//
	// The view-space points are still kept: near-plane clipping needs them
	// for the vertices behind the camera, which have no projection.
	//
	// SOFTWARE-PIPELINED, ONE VERTEX DEEP. The perspective divide is the one
	// long instruction here -- fourteen cycles on the M7 -- and in program
	// order everything after it waits on it. But the M7 lets a VDIV run on
	// while instructions that do not need its result carry on, so each pass
	// round the loop transforms the NEXT vertex and starts its divide first,
	// and only then converts THIS one with the quotient the previous pass
	// left. The divide is then free as long as there is that much other work,
	// and there is: a transform and a conversion.
	//
	// The divide is by max(z, near) rather than z so that a vertex behind the
	// near plane -- whose quotient is never used -- cannot divide by zero.
	// For every vertex whose quotient IS used, z >= near, so max(z, near) is z
	// and the quotient is exactly scale16 / z, as it always was.
	//
	// Two vertices a pass, a and b, each one's divide in flight while the
	// other is converted. One a pass would need the quotient copied from the
	// "next" register to the "current" one at the top of the loop, and that
	// copy is itself an instruction that waits for the divide. The last pass
	// fetches its final vertex again rather than reading past the end: one
	// wasted transform a mesh.
	const xform mv = xform_multiply(&scene->view_xform, matrix);
	const float k_min_z = k_near_z;
	const float scene_scale = scene->scale;
	const float proj_add_x = 1.6666666f * scene->centerx * scene_scale;
	const float proj_add_y = scene->centery * scene_scale;
	float* restrict view_z = scene->tmp_view_z;
	float2* restrict view_xy = scene->tmp_view_xy;
	uint8_t* restrict codes = scene->tmp_vert_codes;
	// Straight from view space to the rasterizer's 28.4, offset included:
	// ((x * scale / z + add) - 0.5) * 16 as one multiply-add per axis.
	const float scale16 = scene_scale * 16.0f;
	const float add16_x = (proj_add_x - 0.5f) * 16.0f;
	const float add16_y = (proj_add_y - 0.5f) * 16.0f;
	uint32_t* restrict fixp = scene->tmp_fix_points;
	const int vcount = mesh->vertex_count;
	int frame = clamp_i(inst->frame, 0, mesh->vertex_frame_count - 1);
	// THE QUOTIENT IS PINNED where it is computed. Left alone, GCC sinks each
	// divide into the branch that uses it -- it is only needed for a vertex in
	// front of the near plane -- which puts it straight back in front of its
	// first use and undoes the whole arrangement. An empty asm that claims to
	// read and write the register costs no instruction and cannot be moved
	// into a branch, so the divide stays where it was written.
#if TARGET_PLAYDATE
#define VTX_PIN(k) __asm__ volatile ("" : "+t"(k))
#else
#define VTX_PIN(k) ((void)0)
#endif
#define VTX_FINISH(i, c, ck)                                                   \
	do {                                                                       \
		view_z[i] = (c).z;                                                     \
		view_xy[i].x = (c).x;                                                  \
		view_xy[i].y = (c).y;                                                  \
		codes[i] = ((c).z >= k_min_z)                                          \
		         ? vert_fix((c).x * (ck) + add16_x, (c).y * (ck) + add16_y, &fixp[i]) \
		         : (uint8_t)VC_NEAR;                                           \
	} while (0)
	if (vcount <= 0)
	{
		// nothing to transform
	}
	else if ((mesh->flags & MeshFlag_FloatVertices) == 0)
	{
		// vertices in fixed point
		const uint32_t* src_vtx = (const uint32_t*)mesh->vertices + frame * vcount;
		const float3 q_step = v3_mul(v3_sub(mesh->bbox_max, mesh->bbox_min), f3(1.0f / 2047.0f, 1.0f / 1023.0f, 1.0f / 2047.0f));
		xform mq;
		mq.ax = v3_mulfl(mv.ax, q_step.x);
		mq.ay = v3_mulfl(mv.ay, q_step.y);
		mq.az = v3_mulfl(mv.az, q_step.z);
		mq.pos = xform_transform_pt(&mv, mesh->bbox_min);
#define VTX_PACKED(bits) vtx_packed(&mq, (bits))
		float3 pa = VTX_PACKED(src_vtx[0]);
		float ka = scale16 / max2f(pa.z, k_min_z);
		int i = 0;
		for (; i + 1 < vcount; i += 2)
		{
			const float3 pb = VTX_PACKED(src_vtx[i + 1]);
			float kb = scale16 / max2f(pb.z, k_min_z);
			VTX_PIN(kb);
			VTX_FINISH(i, pa, ka);
			pa = VTX_PACKED(src_vtx[(i + 2 < vcount) ? i + 2 : i + 1]);
			ka = scale16 / max2f(pa.z, k_min_z);
			VTX_PIN(ka);
			VTX_FINISH(i + 1, pb, kb);
		}
		if (i < vcount)
			VTX_FINISH(i, pa, ka);
#undef VTX_PACKED
	}
	else
	{
		// vertices in floating point
		const float3* src_vtx = (const float3*)mesh->vertices + frame * vcount;
		float3 pa = xform_transform_pt(&mv, src_vtx[0]);
		float ka = scale16 / max2f(pa.z, k_min_z);
		int i = 0;
		for (; i + 1 < vcount; i += 2)
		{
			const float3 pb = xform_transform_pt(&mv, src_vtx[i + 1]);
			float kb = scale16 / max2f(pb.z, k_min_z);
			VTX_PIN(kb);
			VTX_FINISH(i, pa, ka);
			pa = xform_transform_pt(&mv, src_vtx[(i + 2 < vcount) ? i + 2 : i + 1]);
			ka = scale16 / max2f(pa.z, k_min_z);
			VTX_PIN(ka);
			VTX_FINISH(i + 1, pb, kb);
		}
		if (i < vcount)
			VTX_FINISH(i, pa, ka);
	}
#undef VTX_FINISH
#undef VTX_PIN

	// The occluder hook sees the transformed vertices before the cull uses
	// them. See Scene.occluder_hook.
	if (scene->occluder_hook && (inst->flags & MeshDrawFlag_Occluder))
		scene->occluder_hook(scene, inst, scene->occluder_user);

	// The shading inputs first: nothing in them depends on which triangles
	// survive, and the cull loop below queues most triangles itself.
	const bool use_lighting = (inst->flags & (MeshDrawFlag_Solid | MeshDrawFlag_Lighting)) == (MeshDrawFlag_Solid | MeshDrawFlag_Lighting);

	// The light, in the mesh's own space, so each lit triangle is one dot
	// product with its stored normal. See mesh_face_normals for why this is
	// the same number the view-space normal gave, and light_in_model for the
	// transform.
	const float3* normals = use_lighting ? mesh_face_normals(mesh) : NULL;
	const float3 light_m = normals ? light_in_model(&mv, scene->light) : f3(0, 0, 0);
	const bool bluenoise = (scene->tmp_draw_flags & SceneDrawFlag_DitherBluenoise) != 0;
	const uint8_t* shades = face_shades(mesh, inst, normals, light_m, use_lighting, bluenoise);

	// Room in the scene-wide queue for everything this instance can add: a
	// triangle fans into at most six (see order_cap above). Reserved before
	// the cull because the cull now writes the queue directly.
	//
	// realloc, not free-and-malloc: instances earlier in this frame have
	// already written entries AND their keys, and both have to survive a
	// growth in the middle of the frame. tmp_scene_order2 is the radix sort's
	// scratch half and holds nothing between calls.
	if (scene->scene_tri_cap < scene->scene_tri_count + mesh->tri_count * 6)
	{
		int cap = scene->scene_tri_count + mesh->tri_count * 6 + 1024;
		scene->tmp_scene_tris = plat_realloc(scene->tmp_scene_tris, cap * sizeof(scene->tmp_scene_tris[0]));
		scene->tmp_scene_order1 = plat_realloc(scene->tmp_scene_order1, cap * sizeof(scene->tmp_scene_order1[0]));
		plat_free(scene->tmp_scene_order2);
		scene->tmp_scene_order2 = plat_malloc(cap * sizeof(scene->tmp_scene_order2[0]));
		scene->scene_tri_cap = cap;
	}

	// backface / z / screen bounds cull triangles
	//
	// What the common path reads from the scene and the mesh is taken into
	// locals first. Every key it writes is a uint32_t store, which as far as
	// the compiler knows could land on any int-sized field of either struct,
	// so read through the pointers each of these was fetched again after
	// every store: five or six loads a triangle that never change.
	//
	// AND THE QUEUE IS WRITTEN HERE. A triangle that passes is resolved and
	// queued for the scene-wide sort on the spot, while its indices and its
	// three 28.4 points are still in registers -- rather than its key being
	// parked in the order table for a second loop to read back, decode, and
	// fetch the same indices and points again. The entries land in the same
	// order as before, which matters: radixsort_upper16keys is stable, and a
	// sort-proxy decal relies on staying after its host on a tie. Pieces cut
	// by the clippers are resolved as soon as they are emitted, for the same
	// reason.
	//
	// Not with silhouette wires: a triangle's wires depend on whether its
	// neighbours are visible, which is only known once the whole mesh has
	// been culled. That style keeps the two passes.
	const bool fused = !use_silhouette;
	int vis_tri_count = 0;
	scene->clip_count = 0;
	const uint16_t *ibPtr = mesh->tris;
	const uint16_t* const tris = mesh->tris;
	const uint16_t* const proxy = mesh->sort_proxy;
	const float* const vz = scene->tmp_view_z;
	const float sort_bias = scene->tmp_sort_bias;
	uint32_t* const order = scene->tmp_order_table1;
	uint32_t* const vis_mask = scene->tmp_vis_tris_mask;
	const int tri_count = mesh->tri_count;
	// The queue's count and buffers in locals likewise; the count goes back
	// to the scene around anything that reads it from there.
	int queued = scene->scene_tri_count;
	SceneTri* const queue = scene->tmp_scene_tris;
	uint32_t* const queue_keys = scene->tmp_scene_order1;
	const uint16_t inst_flags = (uint16_t)inst->flags;
	// Without silhouettes a wire flag is all edges, or the mesh's own.
	const uint8_t* const wire_src =
		((inst->flags & MeshDrawFlag_Wire) && mesh->wire_flags) ? mesh->wire_flags : NULL;
#define RESOLVE_ARGS mesh, inst, shades, normals, light_m, use_lighting, bluenoise, fixp, order
	for (int i = 0; i < tri_count; ++i, ibPtr += 3)
	{
		uint16_t idx0 = ibPtr[0];
		uint16_t idx1 = ibPtr[1];
		uint16_t idx2 = ibPtr[2];
		const unsigned c0 = codes[idx0], c1 = codes[idx1], c2 = codes[idx2];
		const unsigned c_any = c0 | c1 | c2;

		// Straddling the near plane: clip to it rather than dropping the
		// triangle. Cannot be tested or projected first -- the projection of a
		// vertex behind the eye is meaningless, and its sign is wrong, so the
		// backface test would give a random answer. The clipped pieces get
		// their own tests inside.
		if (c_any & VC_NEAR)
		{
			if (!(c0 & c1 & c2 & VC_NEAR))
			{
				const int from = vis_tri_count;
				vis_tri_count += clip_triangle_near(
					scene, mesh, (uint16_t)i, idx0, idx1, idx2,
					scene_scale, proj_add_x, proj_add_y,
					use_silhouette, vis_tri_count);
				if (fused && vis_tri_count > from)
				{
					scene->scene_tri_count = queued;
					resolve_range(scene, RESOLVE_ARGS, from, vis_tri_count);
					queued = scene->scene_tri_count;
				}
			}
			continue;
		}

		// quick bounds check: all three off the same edge of the screen
		if (c0 & c1 & c2 & VC_SCREEN)
			continue;

		// Near the camera enough to be outside the guard band: the float
		// facing test and then the clipper, as before. See Clipping above.
		// Three quarters of a lap never takes this branch.
		if (c_any & VC_GUARD)
		{
			const float3 p0 = project_pt(view_point(scene, idx0), scene_scale, proj_add_x, proj_add_y);
			const float3 p1 = project_pt(view_point(scene, idx1), scene_scale, proj_add_x, proj_add_y);
			const float3 p2 = project_pt(view_point(scene, idx2), scene_scale, proj_add_x, proj_add_y);
			const float d = (p1.x - p0.x) * (p2.y - p0.y)
			              - (p1.y - p0.y) * (p2.x - p0.x);
			if (d <= 0)
				continue;
			const float3 poly[3] = { p0, p1, p2 };
			const float z = sort_depth(scene, mesh, i, idx0, idx1, idx2);
			const int from = vis_tri_count;
			vis_tri_count += emit_clipped_poly(scene, mesh, (uint16_t)i, poly, 3,
			                                   z, use_silhouette, vis_tri_count);
			if (fused && vis_tri_count > from)
			{
				scene->scene_tri_count = queued;
				resolve_range(scene, RESOLVE_ARGS, from, vis_tri_count);
				queued = scene->scene_tri_count;
			}
			continue;
		}

		// ONLY THE FRONT SIDE, by winding -- on the 28.4 points the fill will
		// draw, in integers. Every corner is inside the guard band here, so
		// a difference is under 23,000 and each product under 2^30: exact,
		// and no float is loaded or compared. It is also the winding the fill
		// assumes when it decides which side the middle vertex is on, so the
		// two can no longer disagree about a sliver.
		const uint32_t f0 = fixp[idx0], f1 = fixp[idx1], f2 = fixp[idx2];
		{
			const int32_t x0 = fix_x(f0), y0 = fix_y(f0);
			const int32_t d = (fix_x(f1) - x0) * (fix_y(f2) - y0)
			                - (fix_y(f1) - y0) * (fix_x(f2) - x0);
			if (d <= 0)
				continue;
		}

		// compute face depth midpoints
		float z = sort_depth_of(vz, sort_bias, tris, proxy, i, idx0, idx1, idx2);
		const uint32_t key = pack_depth_index(z, i);
		++vis_tri_count;

		if (!fused)
		{
			order[vis_tri_count - 1] = key;
			// mark triangle as visible: the silhouette pass reads it
			vis_mask[i >> 5] |= 1u << (i & 31);
			continue;
		}

		// Queued now. The index half of a key is 16 bits, so the queue cannot
		// hold more than 65536 triangles in a frame. Nothing here comes close
		// -- a busy frame on Acropolis survives about 900 -- but the bound is
		// the allocation's and the index's, so it is checked rather than
		// assumed. Past it nothing more is queued, as before.
		if (queued >= 0xFFFF)
			continue;
		const uint8_t shade = shades ? shades[i]
		                    : face_shade_value(scene, mesh, inst, normals, light_m,
		                                       use_lighting, bluenoise, i, idx0, idx1, idx2);
		// The swap of corners 1 and 2 that every unclipped entry gets -- see
		// resolve_range.
		SceneTri* out = &queue[queued];
		scene_tri_put_points(out, f0, f2, f1);
		scene_tri_put_rest(out, shade, wire_src ? wire_src[i] : 0xFF, inst_flags);
		queue_keys[queued] = (key & 0xFFFF0000u) | (uint32_t)queued;
		++queued;
	}
	scene->stats_visible_tris += vis_tri_count;
	scene->scene_tri_count = queued;

	// The silhouette style's second pass. See resolve_range.
	if (!fused)
		resolve_range(scene, RESOLVE_ARGS, 0, vis_tri_count);
#undef RESOLVE_ARGS
}

void scene_add_mesh(Scene* scene, const MeshInstance* inst)
{
	if (scene->inst_count + 1 > scene->inst_cap) {
		scene->inst_cap = scene->inst_count + 32;
		scene->instances = plat_realloc(scene->instances, scene->inst_cap * sizeof(scene->instances[0]));
		plat_free(scene->tmp_inst_order_table1);
		plat_free(scene->tmp_inst_order_table2);
		scene->tmp_inst_order_table1 = plat_malloc(scene->inst_cap * sizeof(scene->tmp_inst_order_table1[0]));
		scene->tmp_inst_order_table2 = plat_malloc(scene->inst_cap * sizeof(scene->tmp_inst_order_table2[0]));
	}
	scene->instances[scene->inst_count] = inst;
	scene->inst_count++;
}

void scene_add_line(Scene* scene, float3 a, float3 b)
{
	if (scene->line_count + 2 > scene->line_cap) {
		scene->line_cap = scene->line_count + 32;
		scene->line_buf = plat_realloc(scene->line_buf, scene->line_cap * sizeof(scene->line_buf[0]));
	}
	scene->line_buf[scene->line_count + 0] = a;
	scene->line_buf[scene->line_count + 1] = b;
	scene->line_count += 2;
}


_Static_assert((int)SceneVertex_Guard == (int)VC_GUARD && (int)SceneVertex_Near == (int)VC_NEAR,
               "render.h's SceneVertexCodes are the instance pass's own outcode bits");

void scene_draw(Scene* scene, uint8_t* buffer, int rowstride, enum SceneDrawFlags draw_flags)
{
	scene_draw_begin(scene, draw_flags);
	scene_draw_collect(scene, 0);
	scene_draw_finish(scene, buffer, rowstride);
}

void scene_draw_begin(Scene* scene, enum SceneDrawFlags draw_flags)
{
	fx_init_patterns();
	scene->stats_visible_tris = 0;
	scene->scene_tri_count = 0;
	// The fill is chosen per scene_draw, and a queued triangle's shade is in
	// the fill's own units -- see SceneTri -- so collection needs to know it.
	scene->tmp_draw_flags = draw_flags;

}

// Collect every instance's triangles. No ordering here: instances are
// visited in submission order and their triangles all land in one queue,
// because sorting instances against each other is the thing this stopped
// doing. An instance's sort_bias still applies: it is folded into every one
// of its triangles' depth keys instead (see sort_depth), so it means the same
// thing it always did without objects having to be the unit of ordering.
void scene_draw_collect(Scene* scene, int first)
{
	for (int i = first < 0 ? 0 : first; i < scene->inst_count; ++i)
	{
		const MeshInstance* inst = scene->instances[i];
		if (!inst->mesh)
			continue;
		collect_mesh_instance(scene, inst);
	}
}

void scene_draw_finish(Scene* scene, uint8_t* buffer, int rowstride)
{
	const enum SceneDrawFlags draw_flags = scene->tmp_draw_flags;

	// One sort, then one drawing pass, back to front over the whole scene.
	radixsort_upper16keys(scene->tmp_scene_order1, scene->tmp_scene_order2, scene->scene_tri_count);
	for (int i = 0; i < scene->scene_tri_count; ++i)
	{
		const SceneTri* t = &scene->tmp_scene_tris[scene->tmp_scene_order1[i] & 0xFFFF];
		draw_scene_tri(buffer, rowstride, t, draw_flags);
	}

	// draw loose lines
	const float scene_scale = scene->scale;
	const float proj_add_x = 1.6666666f * scene->centerx * scene_scale;
	const float proj_add_y = scene->centery * scene_scale;
	for (int i = 0; i < scene->line_count; i += 2)
	{
		float3 view_a = xform_transform_pt(&scene->view_xform, scene->line_buf[i + 0]);
		float3 view_b = xform_transform_pt(&scene->view_xform, scene->line_buf[i + 1]);
		if (view_a.z > 0 && view_b.z > 0)
		{
			view_a.x = view_a.x * scene_scale / view_a.z + proj_add_x;
			view_a.y = view_a.y * scene_scale / view_a.z + proj_add_y;
			view_b.x = view_b.x * scene_scale / view_b.z + proj_add_x;
			view_b.y = view_b.y * scene_scale / view_b.z + proj_add_y;
			draw_black_line(buffer, rowstride, &view_a, &view_b);
		}
	}
	scene->line_count = 0; // clear lines each frame
}

// --------------------------------------------------------------------------

Mesh* mesh_load_from_file(const char* path)
{
	PlatFile* f = plat_file_open_read(path);
	if (!f)
		return NULL;
	typedef struct Header {
		uint32_t magic;
		uint32_t flags;
		uint32_t num_verts;
		uint32_t num_tris;
		uint32_t num_frames;
		float3 bbox_min;
		float3 bbox_max;
	} Header;
	Header header;
	plat_file_read(f, &header, sizeof(header));
	if (header.magic != 0x314d4450) // PDM1
	{
		plat_file_close(f);
		return NULL;
	}
	if (header.num_frames == 0)
		header.num_frames = 1;
	Mesh* mesh = plat_calloc(sizeof(Mesh));
	mesh->vertex_count = header.num_verts;
	mesh->vertex_frame_count = header.num_frames;
	mesh->tri_count = header.num_tris;
	mesh->bbox_min = header.bbox_min;
	mesh->bbox_max = header.bbox_max;
	mesh->center = v3_mulfl(v3_add(mesh->bbox_min, mesh->bbox_max), 0.5f);

	const size_t tri_size = header.num_tris * 3 * sizeof(uint16_t);
	mesh->tris = plat_malloc(tri_size);
	if (plat_file_read(f, mesh->tris, tri_size) != tri_size)
		goto _fail;

	if (header.flags & 1)
	{
		const size_t uv_size = header.num_tris * 3 * sizeof(float2);
		mesh->uvs = plat_malloc(uv_size);
		if (plat_file_read(f, mesh->uvs, uv_size) != uv_size)
			goto _fail;
	}
	else
	{
		mesh->uvs = NULL;
	}

	if (header.flags & 2)
	{
		const size_t col_size = header.num_tris * sizeof(uint8_t);
		mesh->colors = plat_malloc(col_size);
		if (plat_file_read(f, mesh->colors, col_size) != col_size)
			goto _fail;
	}
	else
	{
		mesh->colors = NULL;
	}

	const size_t vtx_size = header.num_verts * header.num_frames * sizeof(uint32_t);
	mesh->vertices = plat_malloc(vtx_size);
	if (plat_file_read(f, mesh->vertices, vtx_size) != vtx_size)
		goto _fail;

	plat_file_close(f);
	return mesh;

_fail:
	mesh_free(mesh);
	plat_file_close(f);
	return NULL;
}

Mesh* mesh_create(int vertex_count, int frame_count, int tri_count, bool uvs, bool colors, bool wire_flags)
{
	if (frame_count < 1)
		frame_count = 1;
	Mesh* mesh = plat_calloc(sizeof(Mesh));
	mesh->vertex_count = vertex_count;
	mesh->vertex_frame_count = frame_count;
	mesh->tri_count = tri_count;
	mesh->flags = MeshFlag_FloatVertices;

	mesh->tris = plat_calloc(mesh->tri_count * 3 * sizeof(uint16_t));
	if (uvs)
		mesh->uvs = plat_calloc(mesh->tri_count * 3 * sizeof(float2));

	if (colors)
		mesh->colors = plat_calloc(mesh->tri_count * sizeof(uint8_t));

	if (wire_flags)
		mesh->wire_flags = plat_calloc(mesh->tri_count * sizeof(uint8_t));

	mesh->vertices = plat_calloc(mesh->vertex_count * mesh->vertex_frame_count * sizeof(float3));
	return mesh;
}

void mesh_free(Mesh* mesh)
{
	if (!mesh)
		return;
	plat_free(mesh->vertices);
	plat_free(mesh->uvs);
	plat_free(mesh->tris);
	plat_free(mesh->colors);
	plat_free(mesh->wire_flags);
	plat_free(mesh->adjacency);
	plat_free(mesh->sort_proxy);
	plat_free(mesh->face_normals);
	plat_free(mesh->face_shade);
	plat_free(mesh);
}

void mesh_instance_init(MeshInstance* inst)
{
	memset(inst, 0, sizeof(*inst));
	inst->tr = xform_identity;
	inst->color = 1.0f;
	inst->flags = MeshDrawFlag_Solid | MeshDrawFlag_Lighting;
}
