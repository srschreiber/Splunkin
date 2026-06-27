"""
Build a mystical rune STONE (a standing weathered monolith) for the base core and export
assets/models/glyphstone.glb. See docs/blender-model-scripting.md.

REVAMP: a smooth, beveled, slightly irregular monolith with carved emissive glyph insets, a
chamfered cap and a glowing crystal, plus small floating rune shards that orbit it. Detail is
added cheaply by JOINing same-material geometry on a bone into one primitive (one draw call),
like make_skeleton.py.

The game places/scales this at the core position and tints the stone by the core's health (the
emissive glyphs/crystal stay lit). It is currently posed statically (pose_model with no layers,
cached once). We ADD an "idle" clip (a slow mystical sway + bob of the monolith, with the rune
shards orbiting and pulsing) for when it gets wired to animate.

Rig (Empties): glyphstone(root) -> { monolith, shard0..2 }
  plinth rides the static root; the monolith (shaft + cap + crystal) sways/bobs; each shardN is
  an orbit pivot at the column center carrying a floating rune crystal.
Conventions: Z-up; ORIGIN AT GROUND (Z=0).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_glyphstone.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

STONE = (0.44, 0.44, 0.49, 1.0)
DARK  = (0.30, 0.30, 0.34, 1.0)
GLYPH = (0.30, 0.92, 1.00, 1.0)   # emissive cyan runes
CORE  = (0.55, 0.95, 1.00, 1.0)   # bright emissive crystal


def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass


def mat(name, rgba, emissive=False, strength=4.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    if b:
        b.inputs["Base Color"].default_value = rgba
        if emissive and "Emission Color" in b.inputs:
            b.inputs["Emission Color"].default_value = rgba
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = strength
    return m


def make_empty(name, parent, loc):
    e = bpy.data.objects.new(name, None)
    e.empty_display_size = 0.2
    bpy.context.collection.objects.link(e)
    if parent:
        e.parent = parent; e.matrix_parent_inverse = Matrix.Identity(4)
    e.location = Vector(loc)
    return e


def _obj(name, mesh, loc, rot, mat_, scale, smooth):
    for p in mesh.polygons: p.use_smooth = smooth
    o = bpy.data.objects.new(name, mesh); bpy.context.collection.objects.link(o)
    o.location = Vector(loc); o.scale = Vector(scale)
    if rot: o.rotation_euler = tuple(math.radians(a) for a in rot)
    o.data.materials.append(mat_)
    return o

def sphere(name, r, loc, mat_, scale=(1, 1, 1), subdiv=2, rot=None):
    bm = bmesh.new(); bmesh.ops.create_icosphere(bm, subdivisions=subdiv, radius=r)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, scale, True)

def box(name, size, loc, mat_, rot=None, bev=0.04):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), False)


def assemble(name, bone, objs):
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs: o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    if len(objs) > 1: bpy.ops.object.join()
    res = bpy.context.view_layer.objects.active
    res.name = name
    res.parent = bone; res.matrix_parent_inverse = Matrix.Identity(4)
    res.location = Vector((0, 0, 0))
    return res


def key_rot(obj, action, frame, euler_deg):
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.rotation_mode = 'XYZ'
    obj.rotation_euler = tuple(math.radians(a) for a in euler_deg)
    obj.keyframe_insert(data_path="rotation_euler", frame=frame)

def key_loc(obj, action, frame, loc):
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.location = Vector(loc)
    obj.keyframe_insert(data_path="location", frame=frame)


MB = 0.60   # monolith bone height (top of the plinth)


def main():
    reset_scene()
    stone, dark, glyph, core = (mat("g_stone", STONE), mat("g_dark", DARK),
                                mat("g_glyph", GLYPH, emissive=True),
                                mat("g_core", CORE, emissive=True, strength=6.0))

    # --- Rig (empties). ---
    root     = make_empty("glyphstone", None, (0, 0, 0))
    monolith = make_empty("monolith",   root, (0, 0, MB))

    # --- PLINTH (static, on root): stepped beveled base + corner blocks. ---
    assemble("base", root, [
        box("plinth0", (2.4, 2.4, 0.24), (0, 0, 0.12), dark, bev=0.05),
        box("plinth1", (2.0, 2.0, 0.22), (0, 0, 0.34), stone, bev=0.06),
        box("plinth2", (1.7, 1.7, 0.16), (0, 0, 0.52), dark, rot=(0, 0, 8), bev=0.05),
    ] + [
        box("corner%d" % i, (0.34, 0.34, 0.5), (math.cos(math.radians(45 + i * 90)) * 0.92,
                                                math.sin(math.radians(45 + i * 90)) * 0.92, 0.25),
            stone, rot=(0, 0, 45 + i * 90), bev=0.06) for i in range(4)
    ])

    # --- MONOLITH (sways in idle): weathered tapering shaft built from a few beveled, slightly
    # rotated/leaning slabs, carved emissive glyph insets on the faces, a chamfered cap and a
    # glowing crystal at the apex. Local Z, bone sits at MB. ---
    seg_z = [0.35, 1.12, 1.82, 2.44]      # local heights up the shaft
    seg_w = [1.34, 1.18, 1.04, 0.90]
    seg_h = [0.84, 0.78, 0.72, 0.64]
    seg_yaw = [3, -4, 6, -3]
    parts = []
    for i in range(4):
        parts.append(box("shaft%d" % i, (seg_w[i], seg_w[i] * 0.86, seg_h[i]),
                         (0, 0, seg_z[i]), stone, rot=(0, 0, seg_yaw[i]), bev=0.07))
        # a dark recessed band between segments
        parts.append(box("band%d" % i, (seg_w[i] * 0.96, seg_w[i] * 0.82, 0.06),
                         (0, 0, seg_z[i] + seg_h[i] / 2), dark, rot=(0, 0, seg_yaw[i]), bev=0.02))
        # carved emissive glyphs: a vertical column + two crossbars on the front/back faces
        half = seg_w[i] / 2 + 0.01
        for fy in (half, -half):
            sgn = 1 if fy > 0 else -1
            parts.append(box("gcol%d_%d" % (i, sgn), (0.05, 0.04, seg_h[i] * 0.5),
                             (0, fy, seg_z[i]), glyph, rot=(0, 0, seg_yaw[i]), bev=0.0))
            for dz in (-0.16, 0.16):
                parts.append(box("gbar%d_%d_%d" % (i, sgn, int(dz * 100)),
                                 (seg_w[i] * 0.42, 0.04, 0.05), (0, fy, seg_z[i] + dz),
                                 glyph, rot=(0, 0, seg_yaw[i]), bev=0.0))
        # carved glyph on the side faces too
        halfx = seg_w[i] * 0.86 / 2 + 0.01
        for fx in (halfx, -halfx):
            sgn = 1 if fx > 0 else -1
            parts.append(box("gside%d_%d" % (i, sgn), (0.04, 0.05, seg_h[i] * 0.45),
                             (fx, 0, seg_z[i]), glyph, rot=(0, 0, seg_yaw[i]), bev=0.0))
    # chamfered cap stack + glowing crystal apex
    parts.append(box("cap0", (0.82, 0.82, 0.22), (0, 0, 2.92), dark, rot=(0, 0, 12), bev=0.06))
    parts.append(box("cap1", (0.56, 0.56, 0.20), (0, 0, 3.12), stone, rot=(0, 0, 45), bev=0.06))
    parts.append(sphere("capdome", 0.26, (0, 0, 3.30), stone, scale=(1.0, 1.0, 0.8), subdiv=2))
    parts.append(box("crystal", (0.26, 0.26, 0.44), (0, 0, 3.62), core, rot=(0, 0, 45), bev=0.02))
    parts.append(sphere("crysglow", 0.17, (0, 0, 3.66), core, subdiv=2))
    assemble("stone_mesh", monolith, parts)

    # --- RUNE SHARDS: 3 small floating crystals orbiting the column on pivot bones. ---
    shards = []
    for i in range(3):
        piv = make_empty("shard%d" % i, root, (0, 0, 1.9 + i * 0.35))
        piv.rotation_euler = (0, 0, math.radians(i * 120))   # stagger start angle
        assemble("rune%d" % i, piv, [
            box("shardcry%d" % i, (0.16, 0.16, 0.30), (1.05, 0, 0), glyph, rot=(20, 0, 45), bev=0.02),
        ])
        shards.append(piv)

    # --- Animation: idle = slow mystical sway/bob of the monolith + orbiting, pulsing shards. ---
    idle = bpy.data.actions.new("idle")
    for f, yaw, tilt, bob in [(1, -5, 1.5, 0.0), (60, 0, 0.0, 0.05), (120, 5, -1.5, 0.0),
                              (180, 0, 0.0, 0.05), (240, -5, 1.5, 0.0)]:
        key_rot(monolith, idle, f, (tilt, 0, yaw))
        key_loc(monolith, idle, f, (0, 0, MB + bob))
    # shards orbit a full turn over the loop, each phase-offset, bobbing up/down as they go.
    for i, piv in enumerate(shards):
        start = i * 120
        for f, frac in [(1, 0.0), (120, 0.5), (240, 1.0)]:
            key_rot(piv, idle, f, (0, 0, start + frac * 360))
        bz = 1.9 + i * 0.35
        for f, dz in [(1, 0.0), (60, 0.18), (120, 0.0), (180, -0.18), (240, 0.0)]:
            key_loc(piv, idle, f, (0, 0, bz + dz))

    # Reset to a neutral rest pose so the exporter bakes an unrotated, centered monolith.
    for obj in (monolith, *shards):
        if obj.animation_data: obj.animation_data.action = None
    monolith.rotation_euler = (0, 0, 0); monolith.location = Vector((0, 0, MB))
    for i, piv in enumerate(shards):
        piv.rotation_euler = (0, 0, math.radians(i * 120))
        piv.location = Vector((0, 0, 1.9 + i * 0.35))

    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(monolith, idle)
    for piv in shards: stash(piv, idle)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "glyphstone.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_glyphstone] wrote", out_path)


if __name__ == "__main__":
    main()
