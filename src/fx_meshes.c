// SPDX-License-Identifier: Unlicense

#include "globals.h"
#include "platform.h"
#include "mathlib.h"
#include "util/pixel_ops.h"
#include "render.h"

#include "fx_meshes.h"

static Scene s_scene;

static bool s_draw_wire = false;
static enum DrawStyle s_draw_style = Draw_Pattern;
static float s_cam_dist = 2.0f;

static Mesh* s_mesh_chocomel;
static MeshInstance s_object_chocomel;
static int s_cur_anim = 0;
static int s_cur_anim_start = 0;
#define ANIM_COUNT 5
static int s_anim_table[ANIM_COUNT+1] = {
	0, // idle sitting
	54, // idle
	60, // walk
	72, // trot
	80, // dig
	110,
};

static void setup_vertex_anim()
{
	s_mesh_chocomel = mesh_load_from_file("chocomel_merged.pdm");
	s_object_chocomel.mesh = s_mesh_chocomel;
	s_object_chocomel.tr = xform_identity;
	s_object_chocomel.frame = 0;
}

void fx_meshes_update()
{
	if (!s_mesh_chocomel)
		return;

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
	if (G.buttons_pressed & kPlatButtonLeft)
	{
		s_draw_style--;
		if (s_draw_style < 0 || s_draw_style >= Draw_Count)
			s_draw_style = Draw_Count - 1;
	}
	if (G.buttons_pressed & kPlatButtonRight)
	{
		s_draw_style++;
		if (s_draw_style < 0 || s_draw_style >= Draw_Count)
			s_draw_style = 0;
	}
	if (G.buttons_pressed & kPlatButtonA)
		s_draw_wire = !s_draw_wire;

	float cangle = G.crank_angle_rad;
	float cs = cosf(cangle);
	float ss = sinf(cangle);
	scene_setCamera(&s_scene, (float3) {
		cs * s_cam_dist, 1.0f, ss * s_cam_dist
	}, (float3) { 0, 0.5f, 0 }, 1.0f, (float3) { 0, -1, 0 });

	// draw
	plat_gfx_clear(kSolidColorWhite);

	scene_drawMesh(&s_scene, G.framebuffer, G.framebuffer_stride, s_object_chocomel.mesh, &s_object_chocomel.tr, s_object_chocomel.frame, s_draw_style, s_draw_wire);

	G.statval1 = s_draw_style;
	G.statval2 = s_draw_wire ? 1 : 0;

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
	s_object_chocomel.frame = s_anim_table[s_cur_anim] + frame;
}

void fx_meshes_init()
{
	setup_vertex_anim();
	scene_init(&s_scene);
	scene_setLight(&s_scene, (float3) { -1.0f, -1.0f, 0.9f });
}
