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

enum ChocomelAnims {
	ChocoAnimIdleSit,
	ChocoAnimIdle,
	ChocoAnimWalk,
	ChocoAnimTrot,
	ChocoAnimDig,
	ChocoAnimCount
};
static enum ChocomelAnims s_choco_cur_anim = ChocoAnimIdleSit;
static int s_choco_cur_anim_start = 0;
static float s_dig_start = -1.0f;
static float s_idle_start = -1.0f;
static int s_choco_anim_table[ChocoAnimCount + 1] = {
	0, // idle sitting
	54, // idle
	60, // walk
	72, // trot
	80, // dig
	110,
};
static void choco_anim_start(enum ChocomelAnims anim)
{
	if (anim == s_choco_cur_anim)
		return;

	// handle dig states
	s_dig_start = (anim == ChocoAnimDig) ? G.t_cur : -1.0f;

	// handle idle states
	if (anim != ChocoAnimIdle && anim != ChocoAnimIdleSit)
	{
		// switching to non-idle animation
		s_idle_start = -1.0f;
	}
	else
	{
		// starting one of idle animations; if current one is not idle then remember
		// idle start time
		if (s_choco_cur_anim != ChocoAnimIdle && s_choco_cur_anim != ChocoAnimIdleSit)
		{
			s_idle_start = G.t_cur;
		}
	}
	s_choco_cur_anim = anim;

	const int cur_frame_idx = (int)(G.t_cur * 24.0f / 2.0f);
	s_choco_cur_anim_start = cur_frame_idx;
}
static void choco_anim_update(MeshInstance* inst)
{
	const int cur_frame_idx = (int)(G.t_cur * 24.0f / 2.0f);
	const int anim_frames = s_choco_anim_table[s_choco_cur_anim + 1] - s_choco_anim_table[s_choco_cur_anim];
	const int frame = (cur_frame_idx - s_choco_cur_anim_start) % anim_frames;
	inst->frame = s_choco_anim_table[s_choco_cur_anim] + frame;
}

// --------------------------------------------------------------------
//  mesh zoo scene

static MeshInstance s_zoo_objects[TestMeshCount];
static Mesh* s_zoo_mesh_ground;
static MeshInstance s_zoo_obj_ground;
static Scene s_zoo_scene;
static float s_zoo_cam_dist = 3.0f;
static float3 s_zoo_obj_pos[TestMeshCount];
static bool s_zoo_bluenoise = false;

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
		mesh_instance_init(go);
		go->mesh = s_meshes[i];
		float x = ((float)i_col - (columns - 1) * 0.5f) * spacing;
		float z = ((float)i_row - (columns - 1) * 0.5f) * spacing;
		min_x = min2f(min_x, x);
		max_x = max2f(max_x, x);
		min_z = min2f(min_z, z);
		max_z = max2f(max_z, z);
		go->tr.pos = s_zoo_obj_pos[i] = f3(x, 0, z);
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
		mesh_instance_init(&s_zoo_obj_ground);
		s_zoo_obj_ground.mesh = m;
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
	const float cur_time = G.t_cur;
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

	choco_anim_update(&s_zoo_objects[TestMesh_Chocomel]);

	// Left: toggle bluenoise vs pattern
	if (G.buttons_pressed & kPlatButtonLeft)
		s_zoo_bluenoise = !s_zoo_bluenoise;
}

// --------------------------------------------------------------------
//  move along a curve scene

static float s_start_time;
static float s_finish_time;
static Scene s_curve_scene;
static Mesh* s_curve_mesh;
static MeshInstance s_curve_obj;
static MeshInstance s_curve_obj_choco;
static MeshInstance s_curve_obj_gate;
static float s_cur_t;
static float s_move_speed;
static float s_jump_height;
static float s_jump_velocity;
static float s_blocked_by_obstacle_start = -1.0f;
static int s_obstacle_counter = 0;

static PlatBitmap* s_img_instructions;
static PlatBitmap* s_img_move;
static PlatBitmap* s_img_dig;
static PlatBitmap* s_img_finished;
static PlatBitmap* s_img_percent;
static PlatBitmap* s_img_digits[10];

typedef struct CurveObstacle
{
	MeshInstance inst;
	float time_start;
	float curve_t;
	float health;
	float3 orig_pos;
} CurveObstacle;

static CurveObstacle s_curve_obstacle;

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

static xform lissajous_curve_frame(float t)
{
	const float delta_t = 0.01f;
	float3 t_pos = lissajous_curve(t);
	float3 tneg_pos = lissajous_curve(t - delta_t);
	float3 tpos_pos = lissajous_curve(t + delta_t);
	float3 dir_fwd = v3_sub(tpos_pos, tneg_pos);
	float3 dir_side = v3_normalize(v3_cross(v3_sub(t_pos, tneg_pos), v3_sub(tpos_pos, t_pos)));
	float3 dir_up = v3_cross(dir_side, dir_fwd);
	xform tr = xform_look_at(tneg_pos, tpos_pos, dir_up);
	tr.pos = t_pos;
	return tr;
}

static void init_curve_scene()
{
	scene_init(&s_curve_scene);
	scene_setLight(&s_curve_scene, f3(-1.0f, 1.0f, -0.1f));
	s_cur_t = 0.001f;

	// load images
	const char* img_err = NULL;
	s_img_instructions = plat_gfx_load_bitmap("img_instructions.pdi", &img_err);
	s_img_dig = plat_gfx_load_bitmap("img_dig.pdi", &img_err);
	s_img_move = plat_gfx_load_bitmap("img_move.pdi", &img_err);
	s_img_finished = plat_gfx_load_bitmap("img_finished.pdi", &img_err);
	s_img_percent = plat_gfx_load_bitmap("img_percent.pdi", &img_err);
	for (int i = 0; i < 10; ++i)
	{
		char name_digit[] = "img_0.pdi";
		name_digit[4] = '0' + i;
		s_img_digits[i] = plat_gfx_load_bitmap(name_digit, &img_err);
	}

	// curve mesh
	{
		int len_segments = 600;
		Mesh* m = mesh_create(2 * len_segments, 1, len_segments * 4, false, true, true);
		float3* dst_pos = (float3*)m->vertices;
		uint16_t* dst_ib = m->tris;
		uint8_t* dst_col = m->colors;
		uint8_t* dst_wire = m->wire_flags;
		for (int i = 0; i < len_segments; ++i) {
			float t = (float)i / (float)len_segments;
			xform curve_tr = lissajous_curve_frame(t);
			dst_pos[0] = v3_sub(curve_tr.pos, curve_tr.ax);
			dst_pos[1] = v3_add(curve_tr.pos, curve_tr.ax);
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
		s_curve_mesh = m;
		mesh_instance_init(&s_curve_obj);
		s_curve_obj.mesh = m;
		s_curve_obj.sort_bias = 1000.0f;
		s_curve_obj.flags |= MeshDrawFlag_Wire;
		scene_add_mesh(&s_curve_scene, &s_curve_obj);
	}

	// chocomel
	{
		mesh_instance_init(&s_curve_obj_choco);
		s_curve_obj_choco.mesh = s_meshes[TestMesh_Chocomel];
		s_curve_obj_choco.flags |= MeshDrawFlag_SilhouetteWire;
		scene_add_mesh(&s_curve_scene, &s_curve_obj_choco);
	}

	// obstacle
	{
		mesh_instance_init(&s_curve_obstacle.inst);
		s_curve_obstacle.inst.flags |= MeshDrawFlag_SilhouetteWire;
		scene_add_mesh(&s_curve_scene, &s_curve_obstacle.inst);
	}

	// start/finish gate
	{
		mesh_instance_init(&s_curve_obj_gate);
		s_curve_obj_gate.mesh = s_meshes[TestMesh_Fence];
		s_curve_obj_gate.flags |= MeshDrawFlag_SilhouetteWire;
		s_curve_obj_gate.tr = lissajous_curve_frame(0.0f);
		scene_add_mesh(&s_curve_scene, &s_curve_obj_gate);
	}
}

typedef struct ObstacleDesc {
	enum TestMeshes mesh;
	float health;
	float max_rotation;
} ObstacleDesc;

static ObstacleDesc k_obstacle_desc[] = {
	{TestMesh_Boulder001, 2.0f, M_PIf},
	{TestMesh_Boulder002, 2.0f, M_PIf},
	{TestMesh_Boulder007, 2.0f, M_PIf},
	{TestMesh_Log003, 3.0f, M_PIf * 0.1f},
	{TestMesh_SnowPiles, 1.0f, M_PIf},
	{TestMesh_TrafficCone, 1.0f, M_PIf},
};
#define OBSTACLE_DESC_COUNT (sizeof(k_obstacle_desc) / sizeof(k_obstacle_desc[0]))

static void update_curve_obstacle(CurveObstacle* ob, bool digging)
{
	if (ob->health <= 0.0f || ob->curve_t + 0.003f < s_cur_t)
	{
		// spawn a new one
		++s_obstacle_counter;
		ob->time_start = G.t_cur;
		ob->curve_t = s_cur_t + 0.020f + RandomFloat01(&G.rng) * 0.050f;
		uint32_t ob_index = XorShift32(&G.rng) % OBSTACLE_DESC_COUNT;
		ob->inst.mesh = s_meshes[k_obstacle_desc[ob_index].mesh];
		ob->health = k_obstacle_desc[ob_index].health;
		xform rot = xform_make_axis_angle((RandomFloat01(&G.rng) * 2.0f - 1.0f) * k_obstacle_desc[ob_index].max_rotation, f3(0,1,0));
		ob->inst.tr = lissajous_curve_frame(ob->curve_t);
		ob->inst.tr = xform_multiply(&ob->inst.tr, &rot);
		ob->orig_pos = ob->inst.tr.pos;
	}

	ob->inst.color = 1.0f;
	ob->inst.tr.pos = ob->orig_pos;
	if (digging)
	{
		ob->health -= G.t_dt;
		int t_frames = (int)(G.t_cur * 24.0f);
		if (t_frames % 4 == 0)
			ob->inst.color = 2.0f;
		ob->inst.tr.pos.x += RandomFloat01(&G.rng) * 0.02f - 0.01f;
		ob->inst.tr.pos.y += RandomFloat01(&G.rng) * 0.02f - 0.01f;
		ob->inst.tr.pos.z += RandomFloat01(&G.rng) * 0.02f - 0.01f;
	}
}

static void update_curve_obstacles(bool digging)
{
	update_curve_obstacle(&s_curve_obstacle, digging);
}

static void draw_and_advance_img(int* x, int y, PlatBitmap* img)
{
	int ww, hh;
	plat_gfx_get_bitmap_size(img, &ww, &hh);
	plat_gfx_draw_bitmap(img, *x, y);
	*x += ww;
}

static void update_curve_scene()
{
	const float k_run_speed = 0.6f;

	const bool is_finished = s_cur_t >= 0.999f;

	// Chocomel movement
	// Right: change move speed
	if ((G.buttons_cur & kPlatButtonRight) && !is_finished)
		s_move_speed += 0.7f * G.t_dt;
	else
		s_move_speed -= 0.3f * G.t_dt;
	s_move_speed = clamp_f(s_move_speed, 0.0f, 1.0f);

	// Determine distance to obstacle
	float cur_dist2_to_obs = 1000.0f;
	if (s_curve_obstacle.health > 0.0f && s_curve_obstacle.curve_t > s_cur_t)
	{
		cur_dist2_to_obs = v3_lensq(v3_sub(s_curve_obj_choco.tr.pos, s_curve_obstacle.orig_pos));
	}
	const float dist_for_stop = 1.0f;
	const float obs_height = 0.5f;
	bool is_obstacle = cur_dist2_to_obs < dist_for_stop * dist_for_stop;

	// Up: jump
	if (s_move_speed >= k_run_speed && (G.buttons_cur & kPlatButtonUp) && s_jump_height <= 0.0f && !is_obstacle && !is_finished)
	{
		s_jump_velocity = 3.0f;
	}
	const float k_jump_gravity = 6.0f;
	s_jump_velocity -= G.t_dt * k_jump_gravity;
	s_jump_velocity = clamp_f(s_jump_velocity, -20.0f, 20.0f);
	s_jump_height += s_jump_velocity * G.t_dt;
	s_jump_height = clamp_f(s_jump_height, 0.0f, 10.0f);


	if (s_jump_height > obs_height)
		is_obstacle = false; // we are jumping over the obstacle, ignore it

	// Is there an obstacle in front? Do not move, and allow dig to start
	bool do_dig = false;
	if (is_obstacle && !is_finished)
	{
		if (s_blocked_by_obstacle_start < 0.0f)
			s_blocked_by_obstacle_start = G.t_cur;

		s_move_speed = 0.0f;
		// B: dig
		if (G.buttons_cur & kPlatButtonB)
			do_dig = true;
	}
	else
	{
		s_blocked_by_obstacle_start = -1.0f;
	}

	// Move based on current speed
	s_cur_t += s_move_speed * G.t_dt * 0.01f;

	// Switch to appropriate animation
	if (s_jump_height > 0.0f)
		choco_anim_start(ChocoAnimTrot);
	else if (do_dig)
		choco_anim_start(ChocoAnimDig);
	else if (s_move_speed >= k_run_speed)
		choco_anim_start(ChocoAnimTrot);
	else if (s_move_speed >= 0.01f)
		choco_anim_start(ChocoAnimWalk);
	else
	{
		if ((s_idle_start >= 0.0f && G.t_cur - s_idle_start > 2.0f) || s_choco_cur_anim == ChocoAnimIdleSit)
			choco_anim_start(ChocoAnimIdleSit);
		else
			choco_anim_start(ChocoAnimIdle);
	}

	xform tr_cur_t = lissajous_curve_frame(s_cur_t);
	xform tr_cam_target = lissajous_curve_frame(s_cur_t + 0.002f);

	update_curve_obstacles(do_dig);
	choco_anim_update(&s_curve_obj_choco);

	xform obj_tr = tr_cur_t;
	obj_tr.pos = v3_add(obj_tr.pos, v3_mulfl(obj_tr.ay, s_jump_height));

	// Crank: orbit camera
	float cam_dist = 2.7f;
	float cam_angle = 0.42f + G.crank_angle_rad;

	float3 cam_pos = v3_add(tr_cam_target.pos, tr_cam_target.ay);
	cam_pos = v3_add(cam_pos, v3_mulfl(tr_cam_target.ax, cosf(cam_angle) * cam_dist));
	cam_pos = v3_add(cam_pos, v3_mulfl(tr_cam_target.az, sinf(cam_angle) * cam_dist));
	float3 cam_target = v3_add(tr_cam_target.pos, v3_mulfl(tr_cam_target.ay, 0.5f));
	scene_setCamera(&s_curve_scene, cam_pos, cam_target, 1.0f, tr_cam_target.ay);

	s_curve_obj_choco.tr = obj_tr;

	//scene_add_line(&s_curve_scene, tr_curve_t.pos, v3_add(tr_curve_t.pos, tr_curve_t.ax));
	//scene_add_line(&s_curve_scene, tr_curve_t.pos, v3_add(tr_curve_t.pos, tr_curve_t.ay));
	//scene_add_line(&s_curve_scene, tr_curve_t.pos, v3_add(tr_curve_t.pos, tr_curve_t.az));

	// draw
	plat_gfx_clear(kSolidColorWhite);
	scene_draw(&s_curve_scene, G.framebuffer, G.framebuffer_stride, 0);

	// images
	// draw score when finished
	if (is_finished)
	{
		if (s_finish_time <= 0.0f)
		{
			s_finish_time = G.t_cur - s_start_time;
		}
		int ww, hh;
		plat_gfx_get_bitmap_size(s_img_finished, &ww, &hh);
		plat_gfx_draw_bitmap(s_img_finished, SCREEN_X / 2, SCREEN_Y / 2 - hh / 2);

		int total_time = (int)(s_finish_time - s_start_time);
		int dig_1000 = (total_time / 1000) % 10;
		int dig_100 = (total_time / 100) % 10;
		int dig_10 = (total_time / 10) % 10;
		int dig_1 = total_time % 10;
		int x = SCREEN_X / 2;
		int y = SCREEN_Y / 2 + hh / 2;
		if (total_time >= 1000)
			draw_and_advance_img(&x, y, s_img_digits[dig_1000]);
		if (total_time >= 100)
			draw_and_advance_img(&x, y, s_img_digits[dig_100]);
		if (total_time >= 10)
			draw_and_advance_img(&x, y, s_img_digits[dig_10]);
		draw_and_advance_img(&x, y, s_img_digits[dig_1]);

	}
	// draw instructions when we're at start
	else if (s_cur_t < 0.002f)
	{
		int ww, hh;
		plat_gfx_get_bitmap_size(s_img_instructions, &ww, &hh);
		plat_gfx_draw_bitmap(s_img_instructions, SCREEN_X / 2, SCREEN_Y / 2 - hh / 2);
	}
	// draw "dig" on first obstacle, or if we're stuck on an obstacle for a bit
	else if (!do_dig && s_blocked_by_obstacle_start >= 0.0f && (s_obstacle_counter == 1 || G.t_cur - s_blocked_by_obstacle_start > 3.0f))
	{
		plat_gfx_draw_bitmap(s_img_dig, SCREEN_X / 2, SCREEN_Y / 2);
	}
	// draw "move" if idling for a while
	else if (s_idle_start >= 0.0f && (G.t_cur - s_idle_start > 3.0f))
	{
		plat_gfx_draw_bitmap(s_img_move, SCREEN_X / 2, SCREEN_Y / 2);
	}

	// percentage of track finished yet
	if (!is_finished)
	{
		int percent = clamp_i((int)(s_cur_t * 100.0f + 0.5f), 0, 100);
		int dig_100 = percent / 100;
		int dig_10 = (percent / 10) % 10;
		int dig_1 = percent % 10;
		int x = 2;
		int y = SCREEN_Y - (12 + 2);
		if (percent >= 100)
			draw_and_advance_img(&x, y, s_img_digits[dig_100]);
		if (percent >= 10)
			draw_and_advance_img(&x, y, s_img_digits[dig_10]);
		draw_and_advance_img(&x, y, s_img_digits[dig_1]);
		draw_and_advance_img(&x, y, s_img_percent);
	}

	// stats
	G.statval1 = s_curve_scene.stats_visible_tris;
	G.statval2 = 0;
}

void app_initialize()
{
	G.rng = 1;
	init_pixel_ops();
	if (!load_meshes())
		return;
	init_zoo_scene();
	init_curve_scene();
}

static bool s_cur_scene_zoo = false;

void app_update()
{
#if SHOW_STATS
	plat_gfx_begin_stats();
#endif

	// time
	G.t_prev = G.t_cur;
	G.t_cur = plat_time_get();
	G.t_dt = clamp_f(G.t_cur - G.t_prev, 0.0f, 0.5f);

	// track inputs
	PlatButtons btCur, btPushed, btRel;
	plat_input_get_buttons(&btCur, &btPushed, &btRel);
	G.buttons_cur = btCur;
	G.buttons_pressed = btPushed;
	G.crank_angle_rad = plat_input_get_crank_angle_rad();

	G.framebuffer = plat_gfx_get_frame();
	G.framebuffer_stride = SCREEN_STRIDE_BYTES;

	// A: switch between scenes
	if (G.buttons_pressed & kPlatButtonA)
		s_cur_scene_zoo = !s_cur_scene_zoo;

	if (s_cur_scene_zoo)
		update_zoo_scene();
	else
		update_curve_scene();

	// draw FPS, stats
#if SHOW_STATS
	plat_gfx_draw_stats(G.statval1, G.statval2);
#endif

	// tell OS that we've updated the whole screen
	plat_gfx_mark_updated_rows(0, SCREEN_Y-1);
}
