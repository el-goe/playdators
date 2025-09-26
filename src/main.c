// SPDX-License-Identifier: Unlicense

#include "globals.h"
#include "mathlib.h"
#include "platform.h"
#include "render.h"
#include "util/pixel_ops.h"

#define SHOW_STATS 1

static Scene s_scene;

static float s_cam_dist = 2.0f;

#define MESH_COUNT 8
static Mesh* s_meshes[MESH_COUNT];

static MeshInstance s_objects[MESH_COUNT];

static int s_cur_anim = 0;
static int s_cur_anim_start = 0;
#define ANIM_COUNT 5
static int s_anim_table[ANIM_COUNT + 1] = {
	0, // idle sitting
	54, // idle
	60, // walk
	72, // trot
	80, // dig
	110,
};

// Lissajous curve that repeats across t=0..1 range, result -20..+20 range.
static float3 lissajous_curve(float t)
{
	const float scale = 20.0f;
	return f3(
		sinf(2.0f * M_PIf * 3.0f * t + M_PIf * 0.25f) * scale,
		sinf(2.0f * M_PIf * 7.0f * t + M_PIf * 0.00f) * scale,
		sinf(2.0f * M_PIf * 5.0f * t + M_PIf * 0.50f) * scale
	);
}

static void lissajous_curve_frame(float t, float3 *out_pos, float3 *out_prev, float3 *out_next, float3* out_side)
{
	const float delta_t = 0.01f;
	float3 t_pos = lissajous_curve(t);
	float3 tneg_pos = lissajous_curve(t - delta_t);
	float3 tpos_pos = lissajous_curve(t + delta_t);
	float3 side = v3_normalize(v3_cross(v3_sub(t_pos, tneg_pos), v3_sub(tpos_pos, t_pos)));
	*out_pos = t_pos;
	*out_side = side;
	*out_prev = tneg_pos;
	*out_next = tpos_pos;
}

static void create_mesh_objects()
{
	s_meshes[0] = mesh_load_from_file("chocomel_merged.pdm");
	s_objects[0].mesh = s_meshes[0];
	s_objects[0].tr = xform_identity;
	s_objects[0].frame = 0;
	s_objects[0].color = 1.0f;
	s_objects[0].flags = MeshDrawFlag_Solid | MeshDrawFlag_Lighting;

	s_meshes[1] = mesh_load_from_file("boulder_007.pdm");
	s_objects[1].mesh = s_meshes[1];
	s_objects[1].tr = xform_make_translate(-2.0f, 0.0f, 2.0f);
	s_objects[1].frame = 0;
	s_objects[1].color = 1.0f;
	s_objects[1].flags = MeshDrawFlag_Solid | MeshDrawFlag_Lighting;

	s_meshes[2] = mesh_load_from_file("bug_hotel.pdm");
	s_objects[2].mesh = s_meshes[2];
	s_objects[2].tr = xform_make_translate(2.0f, 0.0f, 2.0f);
	s_objects[2].frame = 0;
	s_objects[2].color = 1.0f;
	s_objects[2].flags = MeshDrawFlag_Solid | MeshDrawFlag_Lighting;

	s_meshes[3] = mesh_load_from_file("camping_table.pdm");
	s_objects[3].mesh = s_meshes[3];
	s_objects[3].tr = xform_make_translate(2.0f, 0.0f, -2.0f);
	s_objects[3].frame = 0;
	s_objects[3].color = 1.0f;
	s_objects[3].flags = MeshDrawFlag_Solid | MeshDrawFlag_Lighting;

	s_meshes[4] = mesh_load_from_file("snowman.pdm");
	s_objects[4].mesh = s_meshes[4];
	s_objects[4].tr = xform_make_translate(-2.0f, 0.0f, -2.0f);
	s_objects[4].frame = 0;
	s_objects[4].color = 1.0f;
	s_objects[4].flags = MeshDrawFlag_Solid | MeshDrawFlag_Lighting;

	s_meshes[5] = mesh_load_from_file("traffic_cone.pdm");
	s_objects[5].mesh = s_meshes[5];
	s_objects[5].tr = xform_make_translate(0.0f, 0.0f, -1.0f);
	s_objects[5].frame = 0;
	s_objects[5].color = 1.0f;
	s_objects[5].flags = MeshDrawFlag_Solid | MeshDrawFlag_Lighting;

	{
		// procedural grid
		int grid = 4;
		Mesh* m = mesh_create((grid + 1) * (grid + 1), 1, grid * grid * 2, false, true, false);
		float3* dst = (float3*)m->vertices;
		for (int ty = 0; ty < grid + 1; ty++) {
			for (int tx = 0; tx < grid + 1; tx++) {
				dst->x = tx - grid * 0.5f;
				dst->y = 0.0f;
				dst->z = ty - grid * 0.5f;
				dst++;
			}
		}
		for (int ty = 0; ty < grid; ++ty) {
			for (int tx = 0; tx < grid; ++tx) {
				int qidx = ty * grid + tx;
				int base = ty * (grid+1) + tx;
				m->tris[qidx * 6 + 0] = base;
				m->tris[qidx * 6 + 1] = base + grid + 1 + 1;
				m->tris[qidx * 6 + 2] = base + 1;
				m->tris[qidx * 6 + 3] = base;
				m->tris[qidx * 6 + 4] = base + grid + 1;
				m->tris[qidx * 6 + 5] = base + grid + 1 + 1;
				m->colors[qidx * 2 + 0] = m->colors[qidx * 2 + 1] = ((ty ^ tx) & 1) ? 192 : 64;
			}
		}
		s_meshes[6] = m;
		s_objects[6].mesh = m;
		s_objects[6].tr = xform_make_translate(0.0f, 0.0f, 0.0f);
		s_objects[6].frame = 0;
		s_objects[6].color = 1.0f;
		s_objects[6].flags = MeshDrawFlag_Solid | MeshDrawFlag_Lighting;
		s_objects[6].sort_bias = 100.0f;
	}

	{
		// Lissajous curve

		int len_segments = 600;
		Mesh* m = mesh_create(2 * len_segments, 1, len_segments * 4, false, true, true);
		float3* dst_pos = (float3*)m->vertices;
		uint16_t* dst_ib = m->tris;
		uint8_t* dst_col = m->colors;
		uint8_t* dst_wire = m->wire_flags;
		for (int i = 0; i < len_segments; ++i) {
			float t = (float)i / (float)len_segments;
			float3 cpos, cpos_prev, cpos_next, cside;
			lissajous_curve_frame(t, &cpos, &cpos_prev, &cpos_next, &cside);
			cside = v3_mulfl(cside, 1.0f);
			dst_pos[0] = v3_sub(cpos, cside);
			dst_pos[1] = v3_add(cpos, cside);
			int v0 = i * 2;
			int v1 = i * 2 + 1;
			int v2 = (i * 2 + 2) % (2 * len_segments);
			int v3 = (i * 2 + 3) % (2 * len_segments);
			dst_ib[0] = v0;
			dst_ib[1] = v1;
			dst_ib[2] = v3;
			dst_ib[3] = v0;
			dst_ib[4] = v3;
			dst_ib[5] = v2;
			dst_ib[6] = v0;
			dst_ib[7] = v3;
			dst_ib[8] = v1;
			dst_ib[9] = v0;
			dst_ib[10] = v2;
			dst_ib[11] = v3;
			dst_col[0] = dst_col[1] = dst_col[2] = dst_col[3] = (i & 2) ? 220 : 140;
			dst_wire[0] = 2; // front, right edge
			dst_wire[1] = 1; // front, left edge
			dst_wire[2] = 2; // back, right edge
			dst_wire[3] = 4; // back, left edge
			dst_pos += 2;
			dst_ib += 12;
			dst_col += 4;
			dst_wire += 4;
		}
		s_meshes[7] = m;
		s_objects[7].mesh = m;
		s_objects[7].tr = xform_make_translate(0.0f, 0.0f, 0.0f);
		s_objects[7].frame = 0;
		s_objects[7].sort_bias = 1000.0f;
		s_objects[7].color = 1.0f;
		s_objects[7].flags = MeshDrawFlag_Solid | MeshDrawFlag_Wire | MeshDrawFlag_Lighting;
	}


#if 1
	// scene
	for (int i = 0; i < MESH_COUNT - 1; ++i)
		scene_add_mesh(&s_scene, &s_objects[i]);
#else
	// chocomel
	scene_add_mesh(&s_scene, &s_objects[0]);
	// traffic cone
	//scene_add_mesh(&s_scene, &s_objects[5]);
	// curve
	scene_add_mesh(&s_scene, &s_objects[7]);
	// debug
	//scene_add_mesh(&s_scene, &s_objects[8]);
#endif
}

static void meshes_update()
{
	if (G.buttons_cur & kPlatButtonUp)
	{
		s_cam_dist *= 0.9f;
		s_cam_dist = max2f(0.3f, s_cam_dist);
	}
	if (G.buttons_cur & kPlatButtonDown)
	{
		s_cam_dist *= 1.1f;
		s_cam_dist = min2f(5.0f, s_cam_dist);
	}

#if 1
	// crank orbits the camera
	float cangle = G.crank_angle_rad;
	float cs = cosf(cangle);
	float ss = sinf(cangle);
	scene_setCamera(&s_scene, f3(cs * s_cam_dist, 1.0f, ss * s_cam_dist), f3(0, 0.5f, 0), 1.0f, f3(0, 1, 0));
#else
	// time moves along the curve

	static float time_offset = 0;
	if (G.buttons_cur & kPlatButtonLeft)
		time_offset -= 0.001f;
	if (G.buttons_cur & kPlatButtonRight)
		time_offset += 0.001f;
	float cam_t = -plat_time_get() / 300.0f;
	cam_t = -0.3f + time_offset;
	float3 cpos, cpos_prev, cpos_next, cside;
	lissajous_curve_frame(cam_t, &cpos, &cpos_prev, &cpos_next, &cside);
	float3 cup = v3_normalize(v3_cross(cside, v3_sub(cpos_next, cpos)));
	float3 cam_pos = v3_add(cpos, cup);
	scene_setCamera(&s_scene, cam_pos, cpos_next, 1.0f, cup);

	float obj_t = cam_t + 0.003f;
	float3 obj_pos, obj_pos_prev, obj_pos_next, obj_side;
	lissajous_curve_frame(obj_t, &obj_pos, &obj_pos_prev, &obj_pos_next, &obj_side);
	float3 obj_up = v3_normalize(v3_cross(obj_side, v3_sub(obj_pos_next, obj_pos)));
	xform obj_rot = xform_make_axis_angle(G.crank_angle_rad, f3(0, 1, 0));
	xform obj_tr = xform_look_at(obj_pos, cpos, obj_up);
	obj_tr = xform_multiply(&obj_tr, &obj_rot);

	s_objects[5].tr = obj_tr;
	s_objects[0].tr = obj_tr;

	//scene_add_line(&s_scene, obj_tr.pos, v3_add(obj_tr.pos, obj_tr.ay));
#endif

	// draw
	plat_gfx_clear(kSolidColorWhite);

	scene_draw(&s_scene, G.framebuffer, G.framebuffer_stride);

	G.statval1 = 0;
	G.statval2 = 0;

	const float cur_time = plat_time_get();
	const int cur_frame_idx = (int)(cur_time * 24.0f / 2.0f);

	if (G.buttons_pressed & kPlatButtonB)
	{
		s_cur_anim++;
		if (s_cur_anim >= ANIM_COUNT)
			s_cur_anim = 0;
		s_cur_anim_start = cur_frame_idx;
	}

	const int anim_frames = s_anim_table[s_cur_anim + 1] - s_anim_table[s_cur_anim];
	const int frame = (cur_frame_idx - s_cur_anim_start) % anim_frames;
	s_objects[0].frame = s_anim_table[s_cur_anim] + frame;
}

void app_initialize()
{
	init_pixel_ops();
	scene_init(&s_scene);
	scene_setLight(&s_scene, f3(-1.0f, 1.0f, -0.3f));
	create_mesh_objects();
}

void app_update()
{
#if SHOW_STATS
	plat_gfx_begin_stats();
#endif

	// track inputs
	PlatButtons btCur, btPushed, btRel;
	plat_input_get_buttons(&btCur, &btPushed, &btRel);
	G.buttons_cur = btCur;
	G.buttons_pressed = btPushed;
	G.crank_angle_rad = plat_input_get_crank_angle_rad();

	G.framebuffer = plat_gfx_get_frame();
	G.framebuffer_stride = SCREEN_STRIDE_BYTES;

	meshes_update();

	// draw FPS, stats
#if SHOW_STATS
	plat_gfx_draw_stats(G.statval1, G.statval2);
#endif

	// tell OS that we've updated the whole screen
	plat_gfx_mark_updated_rows(0, SCREEN_Y-1);
}
