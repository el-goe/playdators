// SPDX-License-Identifier: Unlicense

#include "globals.h"
#include "platform.h"
#include "mathlib.h"
#include "util/pixel_ops.h"
#include "render.h"

#include "fx_meshes.h"

#include <stdlib.h>

#include "gen_scene_data.h"

static Scene s_scene;

static bool s_draw_wire = false;
static enum DrawStyle s_draw_style = Draw_Pattern;
static float s_cam_dist = 2.0f;

#define MAX_SCENE_OBJECT_COUNT (64)

static int s_mesh_count;
static Mesh* s_meshes;

static float s_object_distances[MAX_SCENE_OBJECT_COUNT];
static int s_object_order[MAX_SCENE_OBJECT_COUNT];

static MeshInstance s_objects[MAX_SCENE_OBJECT_COUNT];
static int s_object_count;

static void setup_vertex_anim()
{
	s_mesh_count = 1;
	s_meshes = plat_malloc(1 * sizeof(Mesh));
	s_meshes->vertex_count = g_vertex_count;
	s_meshes->tri_count = g_index_count / 3;
	s_meshes->tris = g_ib;
	s_meshes->vertices = g_positions[0];
	s_meshes->uvs = g_uv;
	s_meshes->center = (float3){ 0,0,0 };
	s_object_count = 1;
	s_objects[0].mesh = s_meshes;
	s_objects[0].tr = xform_identity;
}

static int CompareZ(const void* a, const void* b)
{
	int ia = *(const int*)a;
	int ib = *(const int*)b;
	float za = s_object_distances[ia];
	float zb = s_object_distances[ib];
	if (za < zb)
		return +1;
	if (za > zb)
		return -1;
	return 0;
}

void fx_meshes_update()
{
	if (G.buttons_cur & kPlatButtonUp)
	{
		s_cam_dist *= 0.9f;
		s_cam_dist = max2f(3.0f, s_cam_dist);
	}
	if (G.buttons_cur & kPlatButtonDown)
	{
		s_cam_dist *= 1.1f;
		s_cam_dist = min2f(20.0f, s_cam_dist);
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

	// sort the objects
	for (int i = 0; i < s_object_count; ++i)
	{
		const Mesh* mesh = s_objects[i].mesh;
		float3 center_local = xform_transform_pt(&s_objects[i].tr, mesh->center);
		float3 center = xform_transform_pt(&s_scene.camera, center_local);
		s_object_distances[i] = center.z;
		s_object_order[i] = i;
	}
	qsort(s_object_order, s_object_count, sizeof(s_object_order[0]), CompareZ);

	// draw
	plat_gfx_clear(kSolidColorWhite);

	for (int i = 0; i < s_object_count; ++i)
	{
		int idx = s_object_order[i];
		scene_drawMesh(&s_scene, G.framebuffer, G.framebuffer_stride, s_objects[idx].mesh, &s_objects[idx].tr, s_draw_style, s_draw_wire);
	}

	G.statval1 = s_draw_style;
	G.statval2 = s_draw_wire ? 1 : 0;

	const float cur_time = plat_time_get();
	const int frame_idx = (int)(cur_time * 24.0f / 2.0f) % g_frame_count;
	s_meshes->vertices = g_positions[frame_idx];
}

void fx_meshes_init()
{
	setup_vertex_anim();
	scene_init(&s_scene);
	scene_setLight(&s_scene, (float3) { -1.0f, -1.0f, 0.9f });
}
