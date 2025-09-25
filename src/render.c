#include "draw_style.h"
#include "render.h"
#include "platform.h"
#include "util/radixsort.h"

// Parts of the scanline rasterizer are originally based on:
// - mini3d from Playdate SDK examples. Created by Dave Hayden on 10/20/15. Copyright © 2015 Panic, Inc. All rights reserved.

FORCE_INLINE void _drawMaskPattern(uint32_t* p, uint32_t mask, uint32_t color)
{
	if ( mask == 0xffffffff )
		*p = color;
	else
		*p = (*p & ~mask) | (color & mask);
}

static void
drawFragment(uint32_t* row, int x1, int x2, uint32_t color)
{
	if ( x2 < 0 || x1 >= SCREEN_X )
		return;
	
	if ( x1 < 0 )
		x1 = 0;
	
	if ( x2 > SCREEN_X )
		x2 = SCREEN_X;
	
	if ( x1 > x2 )
		return;
	
	// Operate on 32 bits at a time
	
	int startbit = x1 % 32;
	uint32_t startmask = swap((1 << (32 - startbit)) - 1);
	int endbit = x2 % 32;
	uint32_t endmask = swap(((1 << endbit) - 1) << (32 - endbit));
	
	int col = x1 / 32;
	uint32_t* p = row + col;

	if ( col == x2 / 32 )
	{
		uint32_t mask = 0;
		
		if ( startbit > 0 && endbit > 0 )
			mask = startmask & endmask;
		else if ( startbit > 0 )
			mask = startmask;
		else if ( endbit > 0 )
			mask = endmask;
		
		_drawMaskPattern(p, mask, color);
	}
	else
	{
		int x = x1;
		
		if ( startbit > 0 )
		{
			_drawMaskPattern(p++, startmask, color);
			x += (32 - startbit);
		}
		
		while ( x + 32 <= x2 )
		{
			_drawMaskPattern(p++, 0xffffffff, color);
			x += 32;
		}
		
		if ( endbit > 0 )
			_drawMaskPattern(p, endmask, color);
	}
}

static inline int32_t slope(float x1, float y1, float x2, float y2, int shift)
{
	float dx = x2-x1;
	float dy = y2-y1;
	
	if ( dy < 1 )
		return (int32_t)(dx * (1<< shift));
	else
		return (int32_t)(dx / dy * (1<<shift));
}

void drawLine(uint8_t* bitmap, int rowstride, const float3* p1, const float3* p2, int thick, const uint8_t pattern[8])
{
	if ( p1->y > p2->y )
	{
		const float3* tmp = p1;
		p1 = p2;
		p2 = tmp;
	}

	int y = (int)p1->y;
	int endy = (int)p2->y;
	
	if ( y >= SCREEN_Y || endy < 0 || min2f(p1->x, p2->x) >= SCREEN_X || max2f(p1->x, p2->x) < 0 )
		return;
	
	int32_t x = (int32_t)(p1->x * (1<<16));
	int32_t dx = slope(p1->x, p1->y, p2->x, p2->y, 16);
	float py = p1->y;
	
	if ( y < 0 )
	{
		x += (int32_t)(-p1->y * dx);
		y = 0;
		py = 0;
	}

	int32_t x1 = (int32_t)(x + dx * (y+1-py));

	while ( y <= endy )
	{
		uint8_t p = pattern[y%8];
		uint32_t color = (p<<24) | (p<<16) | (p<<8) | p;
		
		if ( y == endy )
			x1 = (int32_t)(p2->x * (1<<16));
		
		if ( dx < 0 )
			drawFragment((uint32_t*)&bitmap[y*rowstride], x1>>16, (x>>16) + thick, color);
		else
			drawFragment((uint32_t*)&bitmap[y*rowstride], x>>16, (x1>>16) + thick, color);
		
		if ( ++y == SCREEN_Y )
			break;

		x = x1;
		x1 += dx;
	}
}

// --------------------------------------------------------------------------
// Overall scene rendering


void scene_init(Scene* scene)
{
	scene_setCamera(scene, (float3) { 0, 0, 0 }, (float3) { 0, 0, 1 }, 1.0, (float3) { 0, 1, 0 });
	scene_setLight(scene, (float3) { 0, -1, 0 });

	scene_setCenter(scene, 0.5, 0.5);

	scene->tmp_points_cap = 0;
	scene->tmp_world_points = NULL;
	scene->tmp_proj_points = NULL;
	scene->order_cap = 0;
	scene->tmp_order_table1 = NULL;
	scene->tmp_order_table2 = NULL;
	scene->inst_count = 0;
	scene->inst_cap = 0;
	scene->instances = NULL;
	scene->tmp_inst_order_table1 = NULL;
	scene->tmp_inst_order_table2 = NULL;
}

void scene_shutdown(Scene* scene)
{
	plat_free(scene->tmp_world_points);
	plat_free(scene->tmp_proj_points);
	plat_free(scene->tmp_order_table1);
	plat_free(scene->tmp_order_table2);
	plat_free(scene->instances);
	plat_free(scene->tmp_inst_order_table1);
	plat_free(scene->tmp_inst_order_table2);
}

void scene_setLight(Scene* scene, float3 light)
{
	scene->light = v3_normalize(light);
}

void scene_setCenter(Scene* scene, float x, float y)
{
	scene->centerx = x;
	scene->centery = y;
}

void scene_setCamera(Scene* scene, float3 origin, float3 lookAt, float scale, float3 up)
{
	scene->scale = 240 * scale;

	// calculate forward vector
	float3 fw = v3_sub(lookAt, origin);
	if (v3_lensq(&fw) < 1.0e-6f)
		fw = f3(1, 0, 0);
	else
		fw = v3_normalize(fw);
	// calculate right vector
	float3 rt = v3_cross(up, fw);
	// up is nearly parallel to forward, pick a different "up"
	if (v3_lensq(&rt) < 1.0e-6f) {
		up = fabsf(fw.y) < 0.99f ? f3(0, 1, 0) : f3(1, 0, 0);
		rt = v3_cross(up, fw);
	}
	rt = v3_normalize(rt);
	// calculate final up
	up = v3_cross(fw, rt);

	xform x;
	// rotation matrix, transposed since we need view matrix
	x.m[0][0] = rt.x; x.m[0][1] = rt.y; x.m[0][2] = rt.z;
	x.m[1][0] = up.x; x.m[1][1] = up.y; x.m[1][2] = up.z;
	x.m[2][0] = fw.x; x.m[2][1] = fw.y; x.m[2][2] = fw.z;
	// origin, again transformed since we need view matrix
	x.x = -v3_dot(rt, origin);
	x.y = -v3_dot(up, origin);
	x.z = -v3_dot(fw, origin);
	scene->camera = x;
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

void drawShapeFace(const Scene* scene, uint8_t* bitmap, int rowstride, const float3* p1, const float3* p2, const float3* p3, const float3* normal, const Mesh* mesh, int tri_index, enum DrawStyle style, bool wire)
{
	// lighting
	float v = v3_dot(*normal, scene->light) * 0.5f + 0.5f;
	//v = normal->z * 0.5f + 0.5f;

	if (mesh->colors) {
		v *= mesh->colors[tri_index] * (1.0f / 255.0f);
	}

	// cheap gamma adjust
	//v = v * v;

	if (style == Draw_Pattern)
	{
		int vi = (int)(32.99f * v);
		if (vi > 32)
			vi = 32;
		else if (vi < 0)
			vi = 0;
		const uint8_t* pattern = (const uint8_t*)&patterns[vi];
		draw_tri_pattern(bitmap, rowstride, p1, p2, p3, pattern);
	}
	else if (style == Draw_Bluenoise)
	{
		int col = (int)(v * 255.0f);
		if (col < 0) col = 0;
		if (col > 255) col = 255;
		draw_tri_bluenoise(bitmap, rowstride, p1, p2, p3, col);
	}
	else if (style == Draw_Checker)
	{
		static float dummy_uvs[6] = {0.0f, 0.0f, 0.2f, 0.0f, 0.0f, 0.2f};
		draw_tri_checker(bitmap, rowstride, p1, p2, p3, mesh->uvs ? mesh->uvs + tri_index * 6 : dummy_uvs);
	}

	// wireframe
	if (wire)
	{
		const uint8_t* pattern = (const uint8_t*)&patterns[0];
		drawLine(bitmap, rowstride, p1, p2, 1, pattern);
		drawLine(bitmap, rowstride, p2, p3, 1, pattern);
		drawLine(bitmap, rowstride, p3, p1, 1, pattern);
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
	bits.f = z;
	//bits.u = float_to_half(z) << 16; //@TODO: might be better precision by putting FP16 into upper bits?
	bits.u = float_flip(bits.u);
	// put index into lowest 16 bits
	bits.u = (bits.u & 0xFFFF0000) | index;
	return bits.u;
}

static void draw_mesh(Scene* scene, uint8_t* buffer, int rowstride, const Mesh* mesh, const xform* matrix, int frame, enum DrawStyle style, bool wire)
{
	// temporary buffers
	if (scene->tmp_points_cap < mesh->vertex_count) {
		scene->tmp_points_cap = mesh->vertex_count;
		scene->tmp_world_points = plat_realloc(scene->tmp_world_points, scene->tmp_points_cap * sizeof(scene->tmp_world_points[0]));
		scene->tmp_proj_points = plat_realloc(scene->tmp_proj_points, scene->tmp_points_cap * sizeof(scene->tmp_proj_points[0]));
	}
	if (scene->order_cap < mesh->tri_count) {
		scene->order_cap = mesh->tri_count;
		plat_free(scene->tmp_order_table1);
		plat_free(scene->tmp_order_table2);
		scene->tmp_order_table1 = plat_malloc(scene->order_cap * sizeof(scene->tmp_order_table1[0]));
		scene->tmp_order_table2 = plat_malloc(scene->order_cap * sizeof(scene->tmp_order_table2[0]));
	}

	// transform points
	if (frame < 0)
		frame = 0;
	if (frame >= mesh->vertex_frame_count)
		frame = mesh->vertex_frame_count;
	if ((mesh->flags & MeshFlag_FloatVertices) == 0)
	{
		// vertices in fixed point
		const uint32_t* src_vtx = (const uint32_t*)mesh->vertices + frame * mesh->vertex_count;
		const float3 bbox_min = mesh->bbox_min;
		const float3 bbox_scale = v3_mul(v3_sub(mesh->bbox_max, mesh->bbox_min), (float3) { 1.0f / 2047.0f, 1.0f / 1023.0f, 1.0f / 2047.0f });
		for (int i = 0; i < mesh->vertex_count; ++i)
		{
			const uint32_t v_bits = *src_vtx;
			const uint32_t v_bits_x = v_bits & 2047;
			const uint32_t v_bits_y = (v_bits >> 11) & 1023;
			const uint32_t v_bits_z = v_bits >> 21;
			float3 v_pos = { (float)v_bits_x, (float)v_bits_y, (float)v_bits_z };
			v_pos = v3_add(v3_mul(v_pos, bbox_scale), bbox_min);
			scene->tmp_world_points[i] = xform_transform_pt(&scene->camera, xform_transform_pt(matrix, v_pos));
			++src_vtx;
		}
	}
	else
	{
		// vertices in floating point
		const float3* src_vtx = (const float3*)mesh->vertices + frame * mesh->vertex_count;
		for (int i = 0; i < mesh->vertex_count; ++i)
		{
			scene->tmp_world_points[i] = xform_transform_pt(&scene->camera, xform_transform_pt(matrix, *src_vtx));
			++src_vtx;
		}
	}

	// project points to screen
	const float scene_scale = scene->scale;
	const float proj_add_x = 1.6666666f * scene->centerx * scene_scale;
	const float proj_add_y = scene->centery * scene_scale;
	for (int i = 0; i < mesh->vertex_count; ++i)
	{
		const float3* p = &scene->tmp_world_points[i];
		float3* pp = &scene->tmp_proj_points[i];
		const float pz = p->z;
		if (pz > 0)
		{
			const float inv_z = scene_scale / pz;
			pp->x = p->x * inv_z + proj_add_x;
			pp->y = p->y * inv_z + proj_add_y;
		}
		pp->z = pz;
	}

	// backface / z / screen bounds cull triangles
	int vis_tri_count = 0;
	const uint16_t *ibPtr = mesh->tris;
	for (int i = 0; i < mesh->tri_count; ++i, ibPtr += 3)
	{
		uint16_t idx0 = ibPtr[0];
		uint16_t idx1 = ibPtr[1];
		uint16_t idx2 = ibPtr[2];
		const float3 p0 = scene->tmp_proj_points[idx0];
		const float3 p1 = scene->tmp_proj_points[idx1];
		const float3 p2 = scene->tmp_proj_points[idx2];

		// If any vertex is behind the camera, skip it
		const int any_non_pos = (p0.z <= 0) | (p1.z <= 0) | (p2.z <= 0);
		if (any_non_pos)
			continue;

		// quick bounds check
		float x1 = p0.x;
		float y1 = p0.y;
		float x2 = p1.x;
		float y2 = p1.y;
		float x3 = p2.x;
		float y3 = p2.y;
		if ((x1 < 0 && x2 < 0 && x3 < 0) ||
			(x1 >= SCREEN_X && x2 >= SCREEN_X && x3 >= SCREEN_X) ||
			(y1 < 0 && y2 < 0 && y3 < 0) ||
			(y1 >= SCREEN_Y && y2 >= SCREEN_Y && y3 >= SCREEN_Y))
			continue;

		// only render front side of faces via winding order
		float dx21 = x2 - x1;
		float dy31 = y3 - y1;
		float dx31 = x3 - x1;
		float dy21 = y2 - y1;
		float d = dx21 * dy31 - dy21 * dx31;
		if (d >= 0)
			continue;

		// compute face depth midpoints
		float z = -(scene->tmp_world_points[idx0].z + scene->tmp_world_points[idx1].z + scene->tmp_world_points[idx2].z);
		scene->tmp_order_table1[vis_tri_count] = pack_depth_index(z, i);

		++vis_tri_count;
	}

	// sort faces by z
	radixsort_upper16keys(scene->tmp_order_table1, scene->tmp_order_table2, vis_tri_count);
	const uint32_t* sorted_order = scene->tmp_order_table1;

	// draw faces
	for (int i = 0; i < vis_tri_count; ++i)
	{
		uint16_t fi = sorted_order[i] & 0xFFFF;
		uint16_t idx0 = mesh->tris[fi * 3 + 0];
		uint16_t idx1 = mesh->tris[fi * 3 + 1];
		uint16_t idx2 = mesh->tris[fi * 3 + 2];

		float3 normal = v3_tri_normal(&scene->tmp_world_points[idx0], &scene->tmp_world_points[idx1], &scene->tmp_world_points[idx2]);
		drawShapeFace(scene, buffer, rowstride, &scene->tmp_proj_points[idx0], &scene->tmp_proj_points[idx1], &scene->tmp_proj_points[idx2], &normal, mesh, fi, style, wire);
	}
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

void scene_draw(Scene* scene, uint8_t* buffer, int rowstride, enum DrawStyle style, bool wire)
{
	// sort objects by distance
	const MeshInstance** inst = scene->instances;
	for (int i = 0; i < scene->inst_count; ++i, ++inst)
	{
		const Mesh* mesh = (*inst)->mesh;
		float3 center_local = xform_transform_pt(&(*inst)->tr, mesh->center);
		float3 center = xform_transform_pt(&scene->camera, center_local);
		scene->tmp_inst_order_table1[i] = pack_depth_index(-center.z, i);
	}
	radixsort_upper16keys(scene->tmp_inst_order_table1, scene->tmp_inst_order_table2, scene->inst_count);

	// draw meshes in the sorted order
	for (int i = 0; i < scene->inst_count; ++i)
	{
		uint16_t index = scene->tmp_inst_order_table1[i] & 0xFFFF;
		const MeshInstance* inst = scene->instances[index];
		draw_mesh(scene, buffer, rowstride, inst->mesh, &inst->tr, inst->frame, style, wire);
	}
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
	Mesh* mesh = plat_malloc(sizeof(Mesh));
	mesh->vertex_count = header.num_verts;
	mesh->vertex_frame_count = header.num_frames;
	mesh->tri_count = header.num_tris;
	mesh->bbox_min = header.bbox_min;
	mesh->bbox_max = header.bbox_max;
	mesh->center = v3_mulfl(v3_add(mesh->bbox_min, mesh->bbox_max), 0.5f);
	mesh->flags = 0;

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

Mesh* mesh_create(int vertex_count, int frame_count, int tri_count, bool uvs, bool colors)
{
	if (frame_count < 1)
		frame_count = 1;
	Mesh* mesh = plat_malloc(sizeof(Mesh));
	mesh->vertex_count = vertex_count;
	mesh->vertex_frame_count = frame_count;
	mesh->tri_count = tri_count;
	mesh->bbox_min = (float3){ 0,0,0 };
	mesh->bbox_max = (float3){ 0,0,0 };
	mesh->center = (float3){ 0,0,0 };
	mesh->flags = MeshFlag_FloatVertices;

	mesh->tris = plat_malloc(mesh->tri_count * 3 * sizeof(uint16_t));
	if (uvs)
		mesh->uvs = plat_malloc(mesh->tri_count * 3 * sizeof(float2));
	else
		mesh->uvs = NULL;

	if (colors)
		mesh->colors = plat_malloc(mesh->tri_count * sizeof(uint8_t));
	else
		mesh->colors = NULL;

	mesh->vertices = plat_malloc(mesh->vertex_count * mesh->vertex_frame_count * sizeof(float3));
	return mesh;
}

void mesh_free(Mesh* mesh)
{
	if (!mesh)
		return;
	if (mesh->vertices)
		plat_free(mesh->vertices);
	if (mesh->uvs)
		plat_free(mesh->uvs);
	if (mesh->tris)
		plat_free(mesh->tris);
	plat_free(mesh);
}
