
bl_info = {
    "name": "Playdate Render3D format",
    "author": "Aras Pranckevicius",
    "version": (1, 0, 0),
    "blender": (4, 5, 0),
    "location": "File > Export",
    "description": "Exports scene for Playdate Render3D toy engine",
    "category": "Import-Export",
}

import bpy
import os
import numpy as np
import struct
from bpy.types import Operator
from bpy.props import (BoolProperty, IntProperty)
from bpy_extras.io_utils import ExportHelper

def get_evaluated_mesh(obj, depsgraph):
    eval_obj = obj.evaluated_get(depsgraph)
    me = eval_obj.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    return me, eval_obj

def ensure_tris(me):
    me.calc_loop_triangles()

def collect_mesh_objects(visible_only=True, view_layer=None):
    if view_layer is None:
        view_layer = bpy.context.view_layer
    out = []
    for o in bpy.data.objects:
        if o.type != 'MESH':
            continue
        if visible_only and not o.visible_get(view_layer=view_layer):
            continue
        out.append(o)
    return out

def export_file(filepath, *, visible_only=True, export_uvs=False, frame_step=1, use_scene_range=True, frame_start=None, frame_end=None, report=None):
    scene = bpy.context.scene
    dg = bpy.context.evaluated_depsgraph_get()
    vl = bpy.context.view_layer

    objs = collect_mesh_objects(visible_only, vl)
    if not objs:
        if report: report({'WARNING'}, "No mesh objects to export.")
        return {'CANCELLED'}

    if use_scene_range:
        f0 = scene.frame_start
        f1 = scene.frame_end
    else:
        f0 = frame_start if frame_start is not None else scene.frame_start
        f1 = frame_end   if frame_end   is not None else scene.frame_end
    step = max(1, int(frame_step))
    frame_count = ((f1 - f0) // step) + 1

    # build index buffer, vertex mapping and bounding box on the first frame
    scene.frame_set(f0)
    bpy.context.view_layer.update()

    vertex_offsets = {}
    total_vertices = 0
    tri_sources = []  # (obj, me, eval_obj, vtx_off, vtx_count)

    for o in objs:
        me, eo = get_evaluated_mesh(o, dg)
        ensure_tris(me)
        vertex_offsets[o.name_full] = total_vertices
        vtx_count = len(me.vertices)
        tri_sources.append((o, me, eo, total_vertices, vtx_count))
        total_vertices += vtx_count

    # check whether we have too many verts
    if total_vertices >= 0xFFFF:
        if report:
            report({'ERROR'}, f"Too many vertices (max 65535).")
        return {'CANCELLED'}

    # build index and UV buffers
    unified_indices = []
    unified_uvs = []
    total_tris = 0
    for (o, me, eo, vtx_off, _) in tri_sources:
        uv_data = me.uv_layers.active.data if export_uvs and me.uv_layers.active else None
        loops = me.loops
        for tri in me.loop_triangles:
            li0 = tri.loops[0]
            li1 = tri.loops[1]
            li2 = tri.loops[2]
            i0 = loops[li0].vertex_index + vtx_off
            i1 = loops[li1].vertex_index + vtx_off
            i2 = loops[li2].vertex_index + vtx_off
            unified_indices.extend([i0, i1, i2])
            if export_uvs:
                if uv_data:
                    uv0 = uv_data[li0].uv
                    uv1 = uv_data[li1].uv
                    uv2 = uv_data[li2].uv
                    unified_uvs.extend([float(uv0.x), float(uv0.y), float(uv1.x), float(uv1.y), float(uv2.x), float(uv2.y)])
                else:
                    unified_uvs.extend([0.0, 0.0, 0.0, 0.0, 0.0, 0.0])
        total_tris += len(me.loop_triangles)

    # clear temporary evaluated meshes
    for (_, _, eo, _, _) in tri_sources:
        eo.to_mesh_clear()

    # swizzle matrix (x, z, -y)
    S = np.array([[1, 0, 0],
                  [0, 0, 1],
                  [0, -1, 0]], dtype=np.float32)

    # evaluate mesh vertex positions across frame into one large numpy array
    P = np.empty((frame_count, total_vertices, 3), dtype=np.float32)
    fi = 0
    for frame in range(f0, f1 + 1, step):
        scene.frame_set(frame)
        bpy.context.view_layer.update()
        for (o, _, _, off, cnt) in tri_sources:
            me, eo = get_evaluated_mesh(o, dg)
            ensure_tris(me)
            # fetch vertex positions
            v = np.empty(cnt * 3, dtype=np.float32)
            me.vertices.foreach_get("co", v)
            v = v.reshape(-1, 3)
            # transform to world coordinates
            Mw = np.array(o.matrix_world, dtype=np.float32)
            R = Mw[:3, :3]
            t = Mw[:3, 3]
            w = v @ R.T + t
            P[fi, off:off + cnt, :] = w @ S.T
            eo.to_mesh_clear()
        fi += 1

    # calculate bounding box
    mins = P.min(axis=(0, 1))
    maxs = P.max(axis=(0, 1))
    extent = np.maximum(maxs - mins, 1e-10)

    # quantize positions into 11.10.11 across bounding box range
    bits = np.array([11, 10, 11], dtype=np.int32)
    qmax = (1 << bits) - 1
    scale = (qmax.astype(np.float32) / extent).astype(np.float32)
    Qf = (P - mins) * scale  # (F,V,3)
    Q = np.rint(Qf).astype(np.int32)
    Q[..., 0] = np.clip(Q[..., 0], 0, qmax[0])  # x
    Q[..., 1] = np.clip(Q[..., 1], 0, qmax[1])  # y
    Q[..., 2] = np.clip(Q[..., 2], 0, qmax[2])  # z

    qx = Q[..., 0].astype(np.uint32)
    qy = Q[..., 1].astype(np.uint32)
    qz = Q[..., 2].astype(np.uint32)
    packed = (qx & 0x7FF) | ((qy & 0x3FF) << 11) | ((qz & 0x7FF) << 21) # (F,V)

    # write file
    os.makedirs(os.path.dirname(os.path.abspath(filepath)), exist_ok=True)
    with open(filepath, "wb") as f:
        # header
        flags = 0
        if export_uvs:
            flags |= 1
        f.write(struct.pack("<4sIIII", b"PDM1", flags, total_vertices, total_tris, frame_count))
        f.write(struct.pack("<6f", *(mins.tolist() + maxs.tolist())))

        # index buffer
        for i in unified_indices:
            f.write(struct.pack("<H", i))

        # UV buffer
        if export_uvs:
            for uv in unified_uvs:
                f.write(struct.pack("<f", uv))

        # quantized positions
        packed.astype('<u4').tofile(f)

    if report:
        report({'INFO'}, f"Exported {total_vertices} verts, {total_tris} tris, {frame_count} frames -> {os.path.basename(filepath)}")
    return {'FINISHED'}

class EXPORT_OT_playdate_render3d(Operator, ExportHelper):
    bl_idname = "export_scene.playdate_render3d"
    bl_label = "Export Playdate"
    bl_options = {'PRESET'}

    filename_ext = ".pdm"
    filter_glob: bpy.props.StringProperty(default="*.pdm", options={'HIDDEN'})

    visible_only: BoolProperty(
        name="Visible Only",
        description="Export visible objects only",
        default=True
    )
    export_uvs: BoolProperty(
        name="Export UVs",
        description="Export texture coordinates",
        default=False
    )
    use_scene_range: BoolProperty(
        name="Use Scene Frame Range",
        description="Export frames from scene start/end",
        default=True
    )
    frame_start: IntProperty(
        name="Start",
        default=1,
        min=-1048576,
    )
    frame_end: IntProperty(
        name="End",
        default=250,
        min=-1048576,
    )
    frame_step: IntProperty(
        name="Step",
        default=1,
        min=1,
    )

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False # no animation

        col = layout.column()
        col.prop(self, "visible_only")
        col.prop(self, "export_uvs")
        col.prop(self, "use_scene_range")
        if not self.use_scene_range:
            row = col.row(align=True)
            row.prop(self, "frame_start")
            row.prop(self, "frame_end")
        col.prop(self, "frame_step")

    def execute(self, context):
        if not self.use_scene_range and self.frame_end < self.frame_start:
            self.report({'ERROR'}, "End frame must be >= start frame")
            return {'CANCELLED'}

        return export_file(
            self.filepath,
            visible_only=self.visible_only,
            export_uvs=self.export_uvs,
            frame_step=self.frame_step,
            use_scene_range=self.use_scene_range,
            frame_start=self.frame_start,
            frame_end=self.frame_end,
            report=self.report,
        )

def menu_func_export(self, context):
    self.layout.operator(EXPORT_OT_playdate_render3d.bl_idname,
                         text="Playdate Render 3D (.pdm)")

def register():
    bpy.utils.register_class(EXPORT_OT_playdate_render3d)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)

def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    bpy.utils.unregister_class(EXPORT_OT_playdate_render3d)

if __name__ == "__main__":
    register()
