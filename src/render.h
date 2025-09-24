#pragma once

#include "mathlib.h"

typedef struct Mesh {
	int vertex_count;
	int vertex_frame_count;
	uint32_t* vertices; // each position is 11.10.11 bits fixed point between bbox min & max
	int tri_count;
	uint16_t* tris;
	float* uvs; // 6x float per triangle
	uint8_t* colors; // 1 byte per triangle
	float3 bbox_min;
	float3 bbox_max;
	float3 center;
} Mesh;

Mesh* mesh_load_from_file(const char* path);
void mesh_free(Mesh* mesh);

typedef struct Scene
{
	xform camera;
	float3 light;

	// location of the Z vanishing point on the screen. (0,0) is top left corner, (1,1) is bottom right. Defaults to (0.5,0.5)
	float centerx;
	float centery;

	// display scaling factor. Default is 120, so that the screen extents are (-1.66,1.66)x(-1,1)
	float scale;

	int tmp_points_cap;
	int tmp_faces_cap;
	float3* tmp_world_points;
	float3* tmp_proj_points;
	uint16_t* tmp_order_table;
	float* tmp_dist_table;
} Scene;

typedef struct MeshInstance {
	const Mesh* mesh;
	xform tr;
	int frame;
} MeshInstance;

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
	Draw_Pattern,
	// 58-59ms initial
	// 56-59ms 11.10.11 fixed point positions
	// 55-59ms calc tri normal after culling
	Draw_Bluenoise,
	// 71-77ms initial
	// 69-75ms 11.10.11 fixed point positions
	// 67-74ms calc tri normal after culling
	Draw_Checker,

	Draw_Count
};

void drawShapeFace(const Scene* scene, uint8_t* bitmap, int rowstride, const float3* p1, const float3* p2, const float3* p3, const float3* normal, const Mesh* mesh, int tri_index, enum DrawStyle style, bool wire);
void scene_drawMesh(Scene* scene, uint8_t* buffer, int rowstride, const Mesh* mesh, const xform* matrix, int frame, enum DrawStyle style, bool wire);
