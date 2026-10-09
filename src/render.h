#pragma once

#include "mathlib.h"

// ---------------------------------------------------------------------------
// This is a fork of playdators with four changes, all unconditional:
//
//   1. xform_transform_pt and v3_tri_normal are inlined in mathlib.h. They were
//      out-of-line calls in the per-vertex and per-lit-triangle loops.
//   2. scene_draw folds model x view into one transform per vertex instead of
//      two.
//   3. draw_mesh_instance rejects whole instances against the frustum with a
//      bounding sphere before transforming any of their vertices.
//   4. radixsort_upper16keys falls back to insertion sort below 64 elements.
//
// Together they measured +6% to +36% depending on scene. Only (2) changes
// output at all, and only by floating-point reassociation. See
// playdators-opt.patch and the README in the benchmark repo.
//
// And one change that is a fix rather than an optimisation:
//
//   5. Triangles that straddle the near plane are CLIPPED to it instead of
//      being dropped whole. Upstream drops any triangle with a vertex nearer
//      than k_min_z, which is invisible in a scene viewed from outside and
//      ruinous in one viewed from inside: standing in Virtua Racing's Ocean
//      tunnel, the roof and wall polygons all straddle the eye plane and the
//      tunnel disappears. Measured over a lap it cost 17% of the screen on
//      average and 80% at worst. See the VR project's README.
//
// And a performance pass for the Cortex-M7 (VR project, 2026-09), all output-
// preserving to within float rounding -- see the VR project's README:
//
//   6. Per-face normals are computed once per mesh, in model space, and the
//      light is taken into each instance's model space instead. A lit
//      triangle costs one dot product rather than a cross product, a square
//      root and a divide.
//   7. The vertex loop dequantises, transforms and projects in one pass, with
//      the dequantisation folded into the model-view transform.
//   8. A queued triangle is 16 bytes, not 40: its screen points are stored
//      already in the rasterizer's 28.4 fixed point, and its brightness
//      already as a dither level. The draw pass reads them in depth order --
//      scattered -- so each one is now one cache line instead of two.
//   9. The rasterizer takes those fixed-point points directly and fills spans
//      with shifts and whole-word stores. Filled triangles are unchanged; the
//      wireframe style's lines, which are drawn from the same stored points,
//      can move by a sixteenth of a pixel.
//  10. The transform pass gives each vertex an outcode -- which screen edges
//      and guard-band edges it is past, or that it is behind the near plane
//      -- so the cull tests three bytes per triangle before reading any
//      coordinates.
//
// And a second pass, on the fill and the lit path (2026-09-28):
//
//  11. The edge walk is 16.16 instead of Hecker's exact DDA: one divide per
//      edge instead of two, two registers per edge per row instead of five,
//      the row loops out of line so they stay in registers, and dither rows
//      pre-expanded to words. Edges can be up to n/65536 px off after n rows,
//      which moves a few pixels a frame.
//  12. Every vertex goes straight from view space to 28.4 in the transform
//      pass. The outcodes, the facing test (in integers) and the queued
//      triangle's points all come from that, and the float projected points
//      are gone -- only the guard-band clipper still projects, on demand.
//      The screen reject is now exactly "covers no pixel centre".
//  13. A mesh keeps each face's final shade, rebuilt only when the light in
//      its own space, its instance colour or the fill changes. The course
//      never moves relative to the sun, so lit costs what flat costs; the
//      car, which turns every frame, keeps the per-face path.
// ---------------------------------------------------------------------------

enum MeshFlags {
	MeshFlag_FloatVertices = (1 << 0)
};

enum MeshDrawFlags {
	// Draw filled triangle, using per-instance color,
	// modulated by per-triangle color (if available).
	MeshDrawFlag_Solid = (1 << 0),
	// Also modulates with wrapped diffuse lighting.
	MeshDrawFlag_Lighting = (1 << 1),

	// Draw triangle edges as black lines. If mesh
	// has wire_flags buffer, this is used to determine
	// which lines to draw.
	MeshDrawFlag_Wire = (1 << 2),

	// Draw silhouette edges as black lines.
	// Silhouette is an edge between a front-facing
	// and back-facing triangle, or an edge of a visible triangle
	// that has no neighboring triangles. Mesh triangle adjacency
	// buffer is calculated upon first rendering of that mesh
	// with silhouette wires on.
	MeshDrawFlag_SilhouetteWire = (1 << 3),

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
	uint16_t* adjacency; // 3x entries per triangle, for which triangles are adjacent to them
	// Optional, one per triangle: the triangle whose depth this one sorts on.
	// A road marking or a sign letter borrows the key of the surface it is
	// painted on, so the two tie at every angle instead of crossing as the
	// camera turns; the exporter emits the host first and the radix sort is
	// stable, so the decal stays on top. NULL means every triangle sorts on
	// its own depth, which is what every mesh without the flag does.
	uint16_t* sort_proxy;
	// One unit normal per triangle, in the mesh's own space, as the lighting
	// sees it. Built on the first lit draw (see mesh_face_normals in
	// render.c) and freed with the mesh. NULL until then, and for meshes with
	// more than one vertex frame, which take the per-frame path.
	float3* face_normals;
	// THE FACE'S FINAL SHADE, cached: the dither level (or blue-noise
	// threshold) each triangle resolves to. For a mesh that does not move
	// relative to the light -- every piece of course -- this is the same
	// number every frame, so it is worked out once and read back instead of
	// relit per visible triangle. See face_shades in render.c for when it is
	// trusted. NULL until a mesh has been drawn twice under the same light.
	uint8_t* face_shade;
	float3   shade_light;     // the model-space light it was built for
	float3   shade_seen;      // the light at the last draw, cached or not
	float    shade_color;     // the instance colour it was built for
	uint8_t  shade_mode;      // bit 0 lit, bit 1 blue noise; 0xFF = none
	float3 bbox_min;
	float3 bbox_max;
	float3 center;
	enum MeshFlags flags;
} Mesh;

Mesh* mesh_load_from_file(const char* path);
Mesh* mesh_create(int vertex_count, int frame_count, int tri_count, bool uvs, bool colors, bool wire_flags);
void mesh_free(Mesh* mesh);

typedef struct MeshInstance {
	Mesh* mesh;
	xform tr;
	int frame;
	enum MeshDrawFlags flags;
	float color;
	float sort_bias; // value to add to object distance for sorting (negative pulls to foreground, positive pushes to background)
} MeshInstance;

void mesh_instance_init(MeshInstance *inst);

// One triangle, resolved as far as the rasteriser needs and waiting for the
// scene-wide sort. The points are already in the order the rasterizer wants
// (the 0,2,1 swap is applied when the entry is written) and already in its
// own number format: 28.4 fixed point, with the half-pixel centre offset
// taken off -- exactly what tri_edge_init used to compute from floats on
// every draw. The guard band keeps every coordinate within +/-1400 pixels,
// so 16 bits hold them with room to spare.
//
// Sixteen bytes, one cache line per two entries. The draw pass reads these in
// depth order, which is scattered through the buffer, so the size is what the
// pass costs in misses.
typedef struct SceneTri {
	int16_t x[3], y[3];  // 28.4 fixed point, centre offset applied
	uint8_t shade;       // dither pattern index 0..32, or blue-noise level
	uint8_t wire_flag;
	uint16_t flags;      // the instance's MeshDrawFlags
} SceneTri;

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
	// Each vertex's screen point in the rasterizer's 28.4 fixed point, the
	// half-pixel offset taken off, x in the low half and y in the high --
	// what a queued SceneTri stores. Worked out once per vertex rather than
	// once per corner of every visible triangle, and what the cull's
	// outcodes and facing test are computed from. Meaningless for a vertex
	// with VC_NEAR or VC_GUARD set; those go through the float paths.
	uint32_t* tmp_fix_points;
	// Per-vertex outcodes from the transform pass (VC_* in render.c), so the
	// cull tests three bytes a triangle before it touches a coordinate.
	uint8_t* tmp_vert_codes;

	int order_cap;
	uint32_t* tmp_order_table1;
	uint32_t* tmp_order_table2;

	int vis_tris_mask_cap;
	uint32_t* tmp_vis_tris_mask;

	// Near-plane clipping scratch. A triangle that straddles the near plane
	// is cut into one or two whole triangles, whose projected vertices live
	// here rather than in the per-vertex arrays -- they are new points, with no
	// vertex index of their own. The sort refers to them by an index past the
	// end of the mesh's own triangles; tmp_clip_src maps back to the triangle
	// they came from, for its colour, its adjacency and its visibility bit.
	int       clip_cap;    // capacity in triangles
	int       clip_count;  // used this instance
	float3*   tmp_clip_pts;  // 3 projected points per clipped triangle
	uint16_t* tmp_clip_src;

	// ONE SORT FOR THE WHOLE SCENE, NOT ONE PER INSTANCE.
	//
	// Upstream sorts instances by the depth of their bounding-sphere centre
	// and then draws each one complete, so an instance is an atomic unit of
	// the painter's order: no triangle of one can ever land between two
	// triangles of another. That is right for a scene of separate objects and
	// wrong for a scene that is one continuous surface cut into pieces, which
	// is what a race track is. Two neighbouring track segments interleave in
	// depth all the way through each other, and no ordering of the pair as
	// wholes can be correct.
	//
	// It hides while you look along the road, because segments are laid out
	// along it and their centres then really are in draw order. Turn across
	// the road and it breaks. Measured against a per-pixel z-buffer over the
	// same triangles, wrong pixels looking sideways:
	//
	//     Ocean       yaw 270    8.33%  ->  0.70%
	//     Big Forest  yaw 270    3.83%  ->  0.82%
	//     Acropolis   yaw 270    3.19%  ->  0.15%
	//
	// and 41-65% fewer over a whole lap. Looking straight ahead it changes
	// almost nothing, which is exactly the shape of the bug being fixed.
	//
	// So the instance pass no longer draws. It resolves each surviving
	// triangle down to what the rasteriser needs -- three screen points, a
	// brightness, the flags -- and parks it here. One radix sort over all of
	// them, then one drawing pass. The instance loop keeps its frustum reject
	// and its per-mesh scratch; only the ordering changes.
	float tmp_sort_bias;   // -3 * the current instance's sort_bias
	enum SceneDrawFlags tmp_draw_flags;  // this scene_draw's, while collecting
	int scene_tri_cap;
	int scene_tri_count;
	SceneTri* tmp_scene_tris;
	uint32_t* tmp_scene_order1;
	uint32_t* tmp_scene_order2;

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
