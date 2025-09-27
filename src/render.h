#pragma once

#include "mathlib.h"

enum MeshFlags {
	MeshFlag_FloatVertices = (1 << 0)
};

enum MeshDrawFlags {
	MeshDrawFlag_Solid = (1 << 0),
	MeshDrawFlag_Lighting = (1 << 1),
	MeshDrawFlag_Wire = (1 << 2),

	//@TODO: control for pattern vs bluenoise dither?
	//@TODO: control for UV checker?
};

enum SceneDrawFlags {
	// When set, uses blue noise dithering.
	// When not set, predefined dithering patterns are used (faster).
	SceneDrawFlag_DitherBluenoise = (1 << 0)
};

typedef struct Mesh {
	int vertex_count;
	int vertex_frame_count;
	void* vertices; // each position is either: uint32_t (11.10.11 bits fixed point between bbox min & max), or just regular float3 depending on flags
	int tri_count;
	uint16_t* tris;
	float* uvs; // 6x float per triangle
	uint8_t* colors; // 1 byte per triangle
	uint8_t* wire_flags; // one bit per triangle edge, whether wire for it should be drawn
	float3 bbox_min;
	float3 bbox_max;
	float3 center;
	enum MeshFlags flags;
} Mesh;

Mesh* mesh_load_from_file(const char* path);
Mesh* mesh_create(int vertex_count, int frame_count, int tri_count, bool uvs, bool colors, bool wire_flags);
void mesh_free(Mesh* mesh);

typedef struct MeshInstance {
	const Mesh* mesh;
	xform tr;
	int frame;
	enum MeshDrawFlags flags;
	float color;
	float sort_bias; // value to add to object distance for sorting (negative pulls to foreground, positive pushes to background)
} MeshInstance;

typedef struct Scene
{
	xform camera_xform;
	xform view_xform; // inverse of camera xform
	float3 light; // "towards light" vector in view space

	// location of the Z vanishing point on the screen. (0,0) is top left corner, (1,1) is bottom right. Defaults to (0.5,0.5)
	float centerx;
	float centery;

	// display scaling factor. Default is 120, so that the screen extents are (-1.66,1.66)x(-1,1)
	float scale;

	int tmp_points_cap;
	float3* tmp_view_points;
	float3* tmp_proj_points;

	int order_cap;
	uint32_t* tmp_order_table1;
	uint32_t* tmp_order_table2;

	int inst_count;
	int inst_cap;
	const MeshInstance** instances;
	uint32_t* tmp_inst_order_table1;
	uint32_t* tmp_inst_order_table2;

	int line_count;
	int line_cap;
	float3* line_buf;

	int stats_visible_tris;
} Scene;

void scene_init(Scene* scene);
void scene_shutdown(Scene* scene);
void scene_setCamera(Scene* scene, float3 origin, float3 lookAt, float scale, float3 up);
void scene_setLight(Scene* scene, float3 light);
void scene_setCenter(Scene* scene, float x, float y);

void scene_add_mesh(Scene* scene, const MeshInstance* inst);
void scene_add_line(Scene* scene, float3 a, float3 b);
void scene_draw(Scene* scene, uint8_t* buffer, int rowstride, enum SceneDrawFlags draw_flags);
