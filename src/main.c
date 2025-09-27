// SPDX-License-Identifier: Unlicense

#include "globals.h"
#include "mathlib.h"
#include "platform.h"
#include "render.h"
#include "util/pixel_ops.h"

#define SHOW_STATS 1

enum TestMeshes {
	TestMesh_Boulder001,
	TestMesh_Boulder002,
	TestMesh_Boulder007,
	TestMesh_BugHotel,
	TestMesh_Chocomel,
	TestMesh_CampingTable,
	TestMesh_Fence,
	TestMesh_Log003,
	TestMesh_SnowPiles,
	TestMesh_Snowman,
	TestMesh_TrafficCone,
	TestMesh_BoulderSmall001,
	TestMesh_BoulderSmall002,
	TestMesh_BoulderSmall003,
	TestMeshCount
};

static const char* kTestMeshesNames[TestMeshCount] = {
	"boulder_001.pdm",
	"boulder_002.pdm",
	"boulder_007.pdm",
	"bug_hotel.pdm",
	"chocomel_merged.pdm",
	"camping_table.pdm",
	"fence.pdm",
	"log_003.pdm",
	"snow_piles.pdm",
	"snowman.pdm",
	"traffic_cone.pdm",
	"boulder_small_001.pdm",
	"boulder_small_002.pdm",
	"boulder_small_003.pdm",
};

static Mesh* s_meshes[TestMeshCount];

static bool load_meshes()
{
	for (int i = 0; i < TestMeshCount; ++i)
	{
		Mesh* m = mesh_load_from_file(kTestMeshesNames[i]);
		if (!m)
		{
			plat_sys_log_error("Error: failed to load mesh %s", kTestMeshesNames[i]);
			return false;
		}
		s_meshes[i] = m;
	}
	return true;
}

// --------------------------------------------------------------------
//  mesh zoo scene

static MeshInstance s_zoo_objects[TestMeshCount];
static Mesh* s_zoo_mesh_ground;
static MeshInstance s_zoo_obj_ground;
static Scene s_zoo_scene;
static float s_zoo_cam_dist = 3.0f;
static float3 s_zoo_cam_target = { 0,0,0 };
static float3 s_zoo_obj_pos[TestMeshCount];
static bool s_zoo_bluenoise = false;

static int s_choco_cur_anim = 0;
static int s_choco_cur_anim_start = 0;
#define CHOCOMEL_ANIM_COUNT 5
static int s_choco_anim_table[CHOCOMEL_ANIM_COUNT + 1] = {
	0, // idle sitting
	54, // idle
	60, // walk
	72, // trot
	80, // dig
	110,
};

static void init_zoo_scene()
{
	scene_init(&s_zoo_scene);
	scene_setLight(&s_zoo_scene, f3(-1.0f, 1.0f, -0.1f));

	int columns = (int)sqrtf(TestMeshCount);
	float spacing = 3.0f;
	float min_x = 0, max_x = 0;
	float min_z = 0, max_z = 0;
	for (int i = 0; i < TestMeshCount; ++i)
	{
		int i_col = i % columns;
		int i_row = i / columns;
		MeshInstance* go = &s_zoo_objects[i];
		go->mesh = s_meshes[i];
		go->tr = xform_identity;
		float x = ((float)i_col - (columns - 1) * 0.5f) * spacing;
		float z = ((float)i_row - (columns - 1) * 0.5f) * spacing;
		min_x = min2f(min_x, x);
		max_x = max2f(max_x, x);
		min_z = min2f(min_z, z);
		max_z = max2f(max_z, z);
		go->tr.pos = s_zoo_obj_pos[i] = f3(x, 0, z);
		go->frame = 0;
		go->color = 1.0f;
		go->flags = MeshDrawFlag_Solid | MeshDrawFlag_Lighting;
		// use silhouette wires for Chocomel
		if (i == TestMesh_Chocomel)
			go->flags |= MeshDrawFlag_SilhouetteWire;
		scene_add_mesh(&s_zoo_scene, go);
	}

	// procedural ground plane mesh
	{
		float padding = 1.5f;
		int grid_min_x = (int)floorf(min_x - padding);
		int grid_max_x = (int)ceilf(max_x + padding);
		int grid_min_z = (int)floorf(min_z - padding);
		int grid_max_z = (int)ceilf(max_z + padding);
		int grid_seg_x = grid_max_x - grid_min_x;
		int grid_seg_z = grid_max_z - grid_min_z;
		Mesh* m = mesh_create((grid_seg_x + 1) * (grid_seg_z + 1), 1, grid_seg_x * grid_seg_z * 2, false, true, false);
		float3* dst = (float3*)m->vertices;
		for (int tz = grid_min_z; tz <= grid_max_z; tz++) {
			for (int tx = grid_min_x; tx <= grid_max_x; tx++) {
				dst->x = (float)tx;
				dst->y = 0.0f;
				dst->z = (float)tz;
				dst++;
			}
		}
		for (int tz = 0; tz < grid_seg_z; ++tz) {
			for (int tx = 0; tx < grid_seg_x; ++tx) {
				int qidx = tz * grid_seg_x + tx;
				int base = tz * (grid_seg_x + 1) + tx;
				m->tris[qidx * 6 + 0] = base;
				m->tris[qidx * 6 + 1] = base + grid_seg_x + 1 + 1;
				m->tris[qidx * 6 + 2] = base + 1;
				m->tris[qidx * 6 + 3] = base;
				m->tris[qidx * 6 + 4] = base + grid_seg_x + 1;
				m->tris[qidx * 6 + 5] = base + grid_seg_x + 1 + 1;
				m->colors[qidx * 2 + 0] = m->colors[qidx * 2 + 1] = ((tz ^ tx) & 1) ? 240 : 20;
			}
		}
		s_zoo_mesh_ground = m;
		s_zoo_obj_ground.mesh = m;
		s_zoo_obj_ground.tr = xform_identity;
		s_zoo_obj_ground.frame = 0;
		s_zoo_obj_ground.color = 1.0f;
		s_zoo_obj_ground.flags = MeshDrawFlag_Solid | MeshDrawFlag_Lighting;
		s_zoo_obj_ground.sort_bias = 100.0f;
		scene_add_mesh(&s_zoo_scene, &s_zoo_obj_ground);
	}
}

static void update_zoo_scene()
{
	// up/down: move camera distance
	if (G.buttons_cur & kPlatButtonUp)
	{
		s_zoo_cam_dist *= 0.9f;
		s_zoo_cam_dist = max2f(0.3f, s_zoo_cam_dist);
	}
	if (G.buttons_cur & kPlatButtonDown)
	{
		s_zoo_cam_dist *= 1.1f;
		s_zoo_cam_dist = min2f(10.0f, s_zoo_cam_dist);
	}

	// rotate objects
	const float cur_time = plat_time_get();
	for (int i = 0; i < TestMeshCount; ++i)
	{
		float angle = (float)i + cur_time * 0.1f;
		xform tr = xform_make_axis_angle(angle, f3(0,1,0));
		tr.pos = s_zoo_obj_pos[i];
		s_zoo_objects[i].tr = tr;
	}

	// crank: orbit the camera
	float cangle = G.crank_angle_rad + 0.3f;
	float cs = cosf(cangle);
	float ss = sinf(cangle);
	scene_setCamera(&s_zoo_scene, f3(cs * s_zoo_cam_dist, 2.0f, ss * s_zoo_cam_dist), f3(0, 0.5f, 0), 1.0f, f3(0, 1, 0));

	// draw
	plat_gfx_clear(kSolidColorWhite);
	enum SceneDrawFlags draw_flags = 0;
	if (s_zoo_bluenoise)
		draw_flags |= SceneDrawFlag_DitherBluenoise;
	scene_draw(&s_zoo_scene, G.framebuffer, G.framebuffer_stride, draw_flags);

	G.statval1 = s_zoo_scene.stats_visible_tris;
	G.statval2 = 0;

	// set chocomel animation frame
	const int cur_frame_idx = (int)(cur_time * 24.0f / 2.0f);
	// B: swich animation clip
	if (G.buttons_pressed & kPlatButtonB)
	{
		s_choco_cur_anim++;
		if (s_choco_cur_anim >= CHOCOMEL_ANIM_COUNT)
			s_choco_cur_anim = 0;
		s_choco_cur_anim_start = cur_frame_idx;
	}

	const int anim_frames = s_choco_anim_table[s_choco_cur_anim + 1] - s_choco_anim_table[s_choco_cur_anim];
	const int frame = (cur_frame_idx - s_choco_cur_anim_start) % anim_frames;
	s_zoo_objects[TestMesh_Chocomel].frame = s_choco_anim_table[s_choco_cur_anim] + frame;

	// A: toggle bluenoise vs pattern
	if (G.buttons_pressed & kPlatButtonA)
		s_zoo_bluenoise = !s_zoo_bluenoise;
}

// --------------------------------------------------------------------
//  move along a curve scene


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

static void init_curve_scene()
{
	//@TODO
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
		/*
		s_meshes[7] = m;
		s_objects[7].mesh = m;
		s_objects[7].tr = xform_make_translate(0.0f, 0.0f, 0.0f);
		s_objects[7].frame = 0;
		s_objects[7].sort_bias = 1000.0f;
		s_objects[7].color = 1.0f;
		s_objects[7].flags = MeshDrawFlag_Solid | MeshDrawFlag_Wire | MeshDrawFlag_Lighting;
		*/
	}


	// chocomel
	//scene_add_mesh(&s_scene, &s_objects[0]);
	// curve
	//scene_add_mesh(&s_scene, &s_objects[7]);
}

static void update_curve_scene()
{
#if 0
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

	// time moves along the curve

	//static float time_offset = 0;
	//if (G.buttons_cur & kPlatButtonLeft)
	//	time_offset -= 0.001f;
	//if (G.buttons_cur & kPlatButtonRight)
	//	time_offset += 0.001f;
	float cam_t = -plat_time_get() / 100.0f;
	//cam_t = -0.3f + time_offset;
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
#endif
}

void app_initialize()
{
	init_pixel_ops();
	if (!load_meshes())
		return;
	init_zoo_scene();
	init_curve_scene();
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

	update_zoo_scene();

	// draw FPS, stats
#if SHOW_STATS
	plat_gfx_draw_stats(G.statval1, G.statval2);
#endif

	// tell OS that we've updated the whole screen
	plat_gfx_mark_updated_rows(0, SCREEN_Y-1);
}
