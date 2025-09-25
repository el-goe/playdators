#pragma once

#include "mathlib.h"

enum MeshFlags {
	MeshFlag_FloatVertices = (1 << 0)
};

typedef struct Mesh {
	int vertex_count;
	int vertex_frame_count;
	void* vertices; // each position is either: uint32_t (11.10.11 bits fixed point between bbox min & max), or just regular float3 depending on flags
	int tri_count;
	uint16_t* tris;
	float* uvs; // 6x float per triangle
	uint8_t* colors; // 1 byte per triangle
	float3 bbox_min;
	float3 bbox_max;
	float3 center;
	uint32_t flags; // MeshFlags
} Mesh;

Mesh* mesh_load_from_file(const char* path);
Mesh* mesh_create(int vertex_count, int frame_count, int tri_count, bool uvs, bool colors);
void mesh_free(Mesh* mesh);

typedef struct MeshInstance {
	const Mesh* mesh;
	xform tr;
	int frame;
} MeshInstance;

typedef struct Scene
{
	xform camera; // note: this is "view matrix" i.e. inverse of camera's world transform
	float3 light;

	// location of the Z vanishing point on the screen. (0,0) is top left corner, (1,1) is bottom right. Defaults to (0.5,0.5)
	float centerx;
	float centery;

	// display scaling factor. Default is 120, so that the screen extents are (-1.66,1.66)x(-1,1)
	float scale;

	int tmp_points_cap;
	float3* tmp_world_points;
	float3* tmp_proj_points;

	int order_cap;
	uint32_t* tmp_order_table1;
	uint32_t* tmp_order_table2;

	int inst_count;
	int inst_cap;
	const MeshInstance** instances;
	uint32_t* tmp_inst_order_table1;
	uint32_t* tmp_inst_order_table2;
} Scene;

void drawLine(uint8_t* bitmap, int rowstride, const float3* p1, const float3* p2, int thick, const uint8_t pattern[8]);

void scene_init(Scene* scene);
void scene_shutdown(Scene* scene);
void scene_setCamera(Scene* scene, float3 origin, float3 lookAt, float scale, float3 up);
void scene_setLight(Scene* scene, float3 light);
void scene_setCenter(Scene* scene, float x, float y);

enum DrawStyle {
	// Stats on animated Chocomel (sit idle): 2696 verts, 4588 tris

	// 52-55ms initial
	// 50-53ms 11.10.11 fixed point positions
	// 47-50ms calc tri normal after culling
	// 37-40ms radix sort faces
	Draw_Pattern,
	// 58-59ms initial
	// 56-59ms 11.10.11 fixed point positions
	// 55-59ms calc tri normal after culling
	// 45-49ms radix sort faces
	Draw_Bluenoise,
	// 71-77ms initial
	// 69-75ms 11.10.11 fixed point positions
	// 67-74ms calc tri normal after culling
	// 56-62ms radix sort faces
	Draw_Checker,

	Draw_Count
};

void scene_add_mesh(Scene* scene, const MeshInstance* inst);
void scene_draw(Scene* scene, uint8_t* buffer, int rowstride, enum DrawStyle style, bool wire);
