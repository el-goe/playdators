// SPDX-License-Identifier: Unlicense

#include "globals.h"
#include "mathlib.h"
#include "platform.h"
#include "render.h"
#include "util/pixel_ops.h"
#include <string.h>

#define SHOW_STATS 1

// ======== Meshes used by the game

enum GameMesh {
	GameMesh_Boulder001,
	GameMesh_Boulder002,
	GameMesh_Boulder007,
	GameMesh_Chocomel,
	GameMesh_Fence,
	GameMesh_Log003,
	GameMesh_SnowPiles,
	GameMesh_Snowman,
	GameMesh_TrafficCone,
	GameMeshCount // keep last
};
static const char* kGameMeshNames[GameMeshCount] = {
	"boulder_001.pdm",
	"boulder_002.pdm",
	"boulder_007.pdm",
	"chocomel_merged.pdm",
	"fence.pdm",
	"log_003.pdm",
	"snow_piles.pdm",
	"snowman.pdm",
	"traffic_cone.pdm",
};
static Mesh* s_meshes[GameMeshCount];
static bool load_meshes()
{
	for (int i = 0; i < GameMeshCount; ++i)
	{
		Mesh* m = mesh_load_from_file(kGameMeshNames[i]);
		if (!m)
		{
			plat_sys_log_error("Error: failed to load mesh %s", kGameMeshNames[i]);
			return false;
		}
		s_meshes[i] = m;
	}
	return true;
}

// ======== Images used by the game

enum GameImage {
	GameImage_0,
	GameImage_1,
	GameImage_2,
	GameImage_3,
	GameImage_4,
	GameImage_5,
	GameImage_6,
	GameImage_7,
	GameImage_8,
	GameImage_9,
	GameImage_Dig,
	GameImage_Finished,
	GameImage_Instructions,
	GameImage_Move,
	GameImage_Percent,
	GameImageCount // keep last
};
static const char* kGameImageNames[GameImageCount] = {
	"img_0.pdi",
	"img_1.pdi",
	"img_2.pdi",
	"img_3.pdi",
	"img_4.pdi",
	"img_5.pdi",
	"img_6.pdi",
	"img_7.pdi",
	"img_8.pdi",
	"img_9.pdi",
	"img_dig.pdi",
	"img_finished.pdi",
	"img_instructions.pdi",
	"img_move.pdi",
	"img_percent.pdi",
};
static PlatBitmap* s_images[GameImageCount];
static bool load_images()
{
	for (int i = 0; i < GameImageCount; ++i)
	{
		char* err;
		PlatBitmap* b = plat_gfx_load_bitmap(kGameImageNames[i], &err);
		if (!b)
		{
			plat_sys_log_error("Error: failed to load image %s: %s", kGameImageNames[i], err);
			return false;
		}
		s_images[i] = b;
	}
	return true;
}

// ======== Game state

enum ChocomelAnims {
	ChocoAnimIdleSit,
	ChocoAnimIdle,
	ChocoAnimWalk,
	ChocoAnimTrot,
	ChocoAnimDig,
	ChocoAnimCount
};

typedef struct GameState
{
	enum ChocomelAnims choco_cur_anim;
	int choco_cur_anim_start;
	float t_idle_start;

	float t_game_start;
	float t_game_finish;

	float curve_t;

	float move_speed;

	float jump_cur_height;
	float jump_cur_velocity;

	float t_blocked_start;
	int obstacle_counter;
} GameState;

static GameState GS;

static void game_state_init()
{
	memset(&GS, 0, sizeof(GS));
	GS.choco_cur_anim = ChocoAnimIdleSit;
	GS.t_idle_start = -1.0f;
	GS.t_game_start = -1.0f;
	GS.t_game_finish = -1.0f;
	GS.t_blocked_start = -1.0f;
	GS.curve_t = 0.001f;
}

static const int s_choco_anim_table[ChocoAnimCount + 1] = {
	0, // idle sitting
	54, // idle
	60, // walk
	72, // trot
	80, // dig
	110,
};

static void choco_anim_start(enum ChocomelAnims anim)
{
	if (anim == GS.choco_cur_anim)
		return;

	// handle idle states
	if (anim != ChocoAnimIdle && anim != ChocoAnimIdleSit)
	{
		// switching to non-idle animation
		GS.t_idle_start = -1.0f;
	}
	else
	{
		// starting one of idle animations; if current one is not idle then remember
		// idle start time
		if (GS.choco_cur_anim != ChocoAnimIdle && GS.choco_cur_anim != ChocoAnimIdleSit)
		{
			GS.t_idle_start = G.t_cur;
		}
	}
	GS.choco_cur_anim = anim;

	const int cur_frame_idx = (int)(G.t_cur * 24.0f / 2.0f);
	GS.choco_cur_anim_start = cur_frame_idx;
}

static void choco_anim_update(MeshInstance* inst)
{
	const int cur_frame_idx = (int)(G.t_cur * 24.0f / 2.0f);
	const int anim_frames = s_choco_anim_table[GS.choco_cur_anim + 1] - s_choco_anim_table[GS.choco_cur_anim];
	const int frame = (cur_frame_idx - GS.choco_cur_anim_start) % anim_frames;
	inst->frame = s_choco_anim_table[GS.choco_cur_anim] + frame;
}

Scene s_curve_scene;
Mesh* s_curve_mesh;
MeshInstance s_curve_obj;
MeshInstance s_curve_obj_choco;
MeshInstance s_curve_obj_gate;
static bool s_draw_bluenoise;

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

static void game_initialize()
{
	game_state_init();
	scene_init(&s_curve_scene);
	scene_setLight(&s_curve_scene, f3(-1.0f, 1.0f, -0.1f));

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
		s_curve_obj_choco.mesh = s_meshes[GameMesh_Chocomel];
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
	//@TODO: hide it when not close to start/finish
	{
		mesh_instance_init(&s_curve_obj_gate);
		s_curve_obj_gate.mesh = s_meshes[GameMesh_Fence];
		s_curve_obj_gate.flags |= MeshDrawFlag_SilhouetteWire;
		s_curve_obj_gate.tr = lissajous_curve_frame(0.0f);
		scene_add_mesh(&s_curve_scene, &s_curve_obj_gate);
	}
}

typedef struct ObstacleDesc {
	enum GameMesh mesh;
	float health;
	float max_rotation;
} ObstacleDesc;

static ObstacleDesc k_obstacle_desc[] = {
	{GameMesh_Boulder001, 2.0f, M_PIf},
	{GameMesh_Boulder002, 2.0f, M_PIf},
	{GameMesh_Boulder007, 2.0f, M_PIf},
	{GameMesh_Log003, 3.0f, M_PIf * 0.1f},
	{GameMesh_Snowman, 3.0f, M_PIf},
	{GameMesh_SnowPiles, 1.0f, M_PIf},
	{GameMesh_TrafficCone, 1.0f, M_PIf},
};
#define OBSTACLE_DESC_COUNT (sizeof(k_obstacle_desc) / sizeof(k_obstacle_desc[0]))

static void update_curve_obstacle(CurveObstacle* ob, bool digging)
{
	if (ob->health <= 0.0f || ob->curve_t + 0.003f < GS.curve_t)
	{
		// spawn a new one
		++GS.obstacle_counter;
		ob->time_start = G.t_cur;
		ob->curve_t = GS.curve_t + 0.020f + RandomFloat01(&G.rng) * 0.050f;
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
	*x -= ww;
}

static void draw_number(int number, int x, int y)
{
	do {
		int digit = number % 10;
		number /= 10;
		draw_and_advance_img(&x, y, s_images[GameImage_0 + digit]);
	} while (number > 0);
}

static void update_game()
{
	const float k_run_speed = 0.6f;

	// Game not started yet: any button starts
	if ((GS.t_game_start < 0.0f) && G.buttons_pressed)
	{
		GS.t_game_start = G.t_cur;
	}

	// Determine if game is finished yet
	const bool is_finished = GS.curve_t >= 0.999f;
	if (is_finished)
	{
		if (GS.t_game_finish <= 0.0f)
			GS.t_game_finish = G.t_cur - GS.t_game_start;
		if (G.buttons_pressed) // any button restarts the game
			game_state_init();
	}

	// Chocomel movement
	// Right: change move speed
	if ((G.buttons_cur & kPlatButtonRight) && !is_finished)
		GS.move_speed += 0.7f * G.t_dt;
	else
		GS.move_speed -= 0.3f * G.t_dt;
	GS.move_speed = clamp_f(GS.move_speed, 0.0f, 1.0f);

	// Determine distance to obstacle
	float cur_dist2_to_obs = 1000.0f;
	if (s_curve_obstacle.health > 0.0f && s_curve_obstacle.curve_t > GS.curve_t)
	{
		cur_dist2_to_obs = v3_lensq(v3_sub(s_curve_obj_choco.tr.pos, s_curve_obstacle.orig_pos));
	}
	const float dist_for_stop = 1.0f;
	const float obs_height = 0.5f;
	bool is_obstacle = cur_dist2_to_obs < dist_for_stop * dist_for_stop;

	// Up: jump
	if (GS.move_speed >= k_run_speed && (G.buttons_cur & kPlatButtonUp) && GS.jump_cur_height <= 0.0f && !is_obstacle && !is_finished)
	{
		GS.jump_cur_velocity = 3.0f;
	}
	const float k_jump_gravity = 6.0f;
	GS.jump_cur_velocity -= G.t_dt * k_jump_gravity;
	GS.jump_cur_velocity = clamp_f(GS.jump_cur_velocity, -20.0f, 20.0f);
	GS.jump_cur_height += GS.jump_cur_velocity * G.t_dt;
	GS.jump_cur_height = clamp_f(GS.jump_cur_height, 0.0f, 10.0f);


	if (GS.jump_cur_height > obs_height)
		is_obstacle = false; // we are jumping over the obstacle, ignore it

	// Is there an obstacle in front? Do not move, and allow dig to start
	bool do_dig = false;
	if (is_obstacle && !is_finished)
	{
		if (GS.t_blocked_start < 0.0f)
			GS.t_blocked_start = G.t_cur;

		GS.move_speed = 0.0f;
		// B: dig
		if (G.buttons_cur & kPlatButtonB)
			do_dig = true;
	}
	else
	{
		GS.t_blocked_start = -1.0f;
	}

	// Move based on current speed
	GS.curve_t += GS.move_speed * G.t_dt * 0.01f;

	// Switch to appropriate animation
	if (GS.jump_cur_height > 0.0f)
		choco_anim_start(ChocoAnimTrot);
	else if (do_dig)
		choco_anim_start(ChocoAnimDig);
	else if (GS.move_speed >= k_run_speed)
		choco_anim_start(ChocoAnimTrot);
	else if (GS.move_speed >= 0.01f)
		choco_anim_start(ChocoAnimWalk);
	else
	{
		if ((GS.t_idle_start >= 0.0f && G.t_cur - GS.t_idle_start > 2.0f) || GS.choco_cur_anim == ChocoAnimIdleSit)
			choco_anim_start(ChocoAnimIdleSit);
		else
			choco_anim_start(ChocoAnimIdle);
	}

	xform tr_cur_t = lissajous_curve_frame(GS.curve_t);
	xform tr_cam_target = lissajous_curve_frame(GS.curve_t + 0.002f);

	update_curve_obstacles(do_dig);
	choco_anim_update(&s_curve_obj_choco);

	xform obj_tr = tr_cur_t;
	obj_tr.pos = v3_add(obj_tr.pos, v3_mulfl(obj_tr.ay, GS.jump_cur_height));

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
	enum SceneDrawFlags draw_flags = 0;
	if (s_draw_bluenoise)
		draw_flags |= SceneDrawFlag_DitherBluenoise;
	scene_draw(&s_curve_scene, G.framebuffer, G.framebuffer_stride, draw_flags);

	// images
	// "finished" label
	if (is_finished)
	{
		int x = SCREEN_X - 3;
		int y = SCREEN_Y - 14;
		int ww, hh;
		plat_gfx_get_bitmap_size(s_images[GameImage_Finished], &ww, &hh);
		plat_gfx_draw_bitmap(s_images[GameImage_Finished], x - ww, y);
	}
	// draw instructions when game has not started yet
	else if (GS.t_game_start < 0.0f)
	{
		int ww, hh;
		plat_gfx_get_bitmap_size(s_images[GameImage_Instructions], &ww, &hh);
		plat_gfx_draw_bitmap(s_images[GameImage_Instructions], SCREEN_X / 2, SCREEN_Y / 2 - hh / 2);
	}
	// draw "dig" on first obstacle, or if we're stuck on an obstacle for a bit
	else if (!do_dig && GS.t_blocked_start >= 0.0f && (GS.obstacle_counter == 1 || G.t_cur - GS.t_blocked_start > 3.0f))
	{
		plat_gfx_draw_bitmap(s_images[GameImage_Dig], SCREEN_X / 2, SCREEN_Y / 2);
	}
	// draw "move" if idling for a while
	else if (GS.t_idle_start >= 0.0f && (G.t_cur - GS.t_idle_start > 3.0f))
	{
		plat_gfx_draw_bitmap(s_images[GameImage_Move], SCREEN_X / 2, SCREEN_Y / 2);
	}

	// draw percentage of track finished yet
	if (GS.t_game_start >= 0.0f && !is_finished)
	{
		int percent = clamp_i((int)(GS.curve_t * 100.0f + 0.5f), 0, 100);
		int x = SCREEN_X - 15;
		int y = SCREEN_Y - 14;
		draw_and_advance_img(&x, y, s_images[GameImage_Percent]);
		draw_number(percent, x, y);
	}

	// draw current/final score
	if (GS.t_game_start >= 0.0f)
	{
		const int score = (int)((is_finished ? GS.t_game_finish : G.t_cur) - GS.t_game_start);
		draw_number(score, SCREEN_X - 15, SCREEN_Y - 28);
	}

	// A: toggle bluenoise vs pattern
	if (G.buttons_pressed & kPlatButtonA)
		s_draw_bluenoise = !s_draw_bluenoise;

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
	if (!load_images())
		return;
	game_initialize();
}

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

	update_game();

	// draw FPS, stats
#if SHOW_STATS
	plat_gfx_draw_stats(G.statval1, G.statval2);
#endif

	// tell OS that we've updated the whole screen
	plat_gfx_mark_updated_rows(0, SCREEN_Y-1);
}
