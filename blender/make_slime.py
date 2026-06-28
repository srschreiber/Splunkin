"""
Procedurally build a SLIME and export assets/models/slime.glb.

A gelatinous green blob creature that oozes along the lane. Cute-but-gross: a wide
gooey dome of bright sickly-green slime, a smaller wobble-bump on top, two dark
beady eyes with little white glints, a few goo drips around the base, and a glossy
pale-green highlight on the dome to read as wet/translucent.

KEY ENGINE FACTS (see docs/blender-model-scripting.md):
  * No GPU skinning. Each glTF *primitive* is a rigid "part" pinned to the NODE that
    holds the mesh, drawn at placement * partNodeWorld. The rig is a hierarchy of
    mesh-less EMPTIES; meshes are parented to those bones.
  * A glTF primitive == one (mesh, material) pair, so ALL same-material geometry
    parented to one bone collapses into ONE draw call -> ~5 prims total here.
  * Clips routed by name: idle / walk / punch. `idle` is the looping base layer the
    engine plays when standing still; `walk` replaces it when moving.

ORIENTATION / SCALE:
  * Z up in Blender (exporter -> Y-up). FACES +Y (eyes toward +Y).
  * Origin at the CENTER of the footprint at GROUND level: the blob's base sits at
    z=0 and bulges up in +Z. Footprint ~1.25 wide, ~0.85 tall.

RIG (mesh-less empties):
  body(root, the whole jiggly mass) -> top(the upper dome bump, wobbles separately)
The slime has no legs: "walk" is a bouncing squash-and-stretch ooze.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_slime.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

GREEN  = (0.32, 0.82, 0.18, 1.0)   # bright sickly-green slime body
GLOW   = (0.55, 0.95, 0.30, 1.0)   # pale-green glossy highlight (wet/translucent read)
DARK   = (0.03, 0.03, 0.05, 1.0)   # beady eyes
WHITE  = (1.0, 1.0, 1.0, 1.0)      # eye glints
TOP_Z  = 0.46                      # top-bump bone height


def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass


def mat(name, rgba, emissive=False, strength=1.2):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    if b:
        b.inputs["Base Color"].default_value = rgba
        if "Roughness" in b.inputs: b.inputs["Roughness"].default_value = 0.15  # glossy goo
        if emissive and "Emission Color" in b.inputs:
            b.inputs["Emission Color"].default_value = rgba
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = strength
    return m


def make_empty(name, parent, loc):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = 'ARROWS'; e.empty_display_size = 0.12
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

def key_scale(obj, action, frame, scale):
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.scale = Vector(scale)
    obj.keyframe_insert(data_path="scale", frame=frame)


def main():
    reset_scene()
    green = mat("slime",     GREEN, emissive=True, strength=0.5)
    glow  = mat("highlight", GLOW,  emissive=True, strength=0.9)
    dark  = mat("eye",       DARK)
    white = mat("glint",     WHITE, emissive=True, strength=2.0)

    # --- Rig (empties). FRONT faces +Y. Origin at ground center. ---
    body = make_empty("body", None, (0, 0, 0.0))
    top  = make_empty("top",  body, (0, 0, TOP_Z))

    # --- BODY (root bone): wide flattened gooey dome + a few goo drips around the base.
    # All bright-green -> one primitive. Dome radius 0.6, squashed in Z so it sits low and
    # wide; centered at z so the base just kisses ground (z=0). ---
    body_green = [
        # main dome (squashed icosphere); base ~z0, top ~z0.74, width ~1.26
        sphere("dome", 0.60, (0, 0.02, 0.39), green, scale=(1.05, 1.02, 0.66), subdiv=3),
        # a fat belly bulge front-low for gooey volume
        sphere("belly", 0.34, (0, 0.30, 0.20), green, scale=(1.1, 0.9, 0.7), subdiv=2),
        # goo drips / oozing blobs creeping out around the base
        sphere("drip0", 0.16, (0.46, 0.18, 0.08), green, scale=(1.0, 1.1, 0.6), subdiv=2),
        sphere("drip1", 0.13, (-0.42, 0.26, 0.07), green, scale=(1.0, 1.0, 0.6), subdiv=2),
        sphere("drip2", 0.14, (-0.30, -0.34, 0.07), green, scale=(1.1, 1.0, 0.55), subdiv=2),
        sphere("drip3", 0.11, (0.30, -0.36, 0.06), green, scale=(1.0, 1.0, 0.55), subdiv=2),
        sphere("drip4", 0.10, (0.52, -0.10, 0.05), green, scale=(0.9, 1.1, 0.5), subdiv=2),
    ]
    assemble("ooze", body, body_green)

    # glossy pale-green highlight smear on the upper-left of the dome (wet sheen)
    assemble("sheen", body, [
        sphere("sheenA", 0.18, (-0.22, -0.14, 0.55), glow, scale=(1.4, 1.0, 0.5), subdiv=2),
        sphere("sheenB", 0.07, (0.10, -0.30, 0.50), glow, scale=(1.2, 1.0, 0.6), subdiv=1),
    ])

    # --- EYES (on body, facing +Y): dark beady spheres pressed into the front of the dome ---
    assemble("eyes", body, [
        sphere("eyeL", 0.10, (0.17, 0.50, 0.44), dark, scale=(1.0, 0.9, 1.05), subdiv=2),
        sphere("eyeR", 0.10, (-0.17, 0.50, 0.44), dark, scale=(1.0, 0.9, 1.05), subdiv=2),
    ])
    # little white glints in each eye
    assemble("glints", body, [
        sphere("glintL", 0.032, (0.21, 0.575, 0.49), white, subdiv=1),
        sphere("glintR", 0.032, (-0.13, 0.575, 0.49), white, subdiv=1),
    ])

    # --- TOP BUMP (top bone): a smaller dome blob that wobbles on top of the mass ---
    assemble("bump", top, [
        sphere("bumpDome", 0.28, (0, -0.02, 0.05), green, scale=(1.05, 1.0, 0.85), subdiv=2),
        sphere("bumpSheen", 0.09, (-0.10, -0.12, 0.18), glow, scale=(1.3, 1.0, 0.5), subdiv=1),
    ])

    # ================= Animations (squash & stretch wobble) =================
    # idle: a slow gelatinous jiggle in place (volume-preserving squash/stretch) + the
    # top bump counter-wobbles a beat behind so it looks like loose goo.
    idle = bpy.data.actions.new("idle")
    for frame, sx, sz, dz in [(1, 1.00, 1.00, 0.00), (15, 0.96, 1.08, 0.015),
                              (30, 1.07, 0.88, -0.02), (45, 0.98, 1.04, 0.01),
                              (60, 1.00, 1.00, 0.00)]:
        key_scale(body, idle, frame, (sx, sx, sz))
        key_loc(body, idle, frame, (0, 0, dz))
    for frame, p, r in [(1, 0, 0), (15, -4, 3), (30, 5, -4), (45, -3, 2), (60, 0, 0)]:
        key_rot(top, idle, frame, (p, 0, r))

    # walk: a bouncing ooze — stretch as it leaps up, big squash on the landing, settle.
    walk = bpy.data.actions.new("walk")
    for frame, sx, sz, dz in [(1, 1.00, 1.00, 0.00), (7, 0.86, 1.22, 0.16),
                              (15, 1.22, 0.74, 0.00), (20, 0.97, 1.05, 0.02),
                              (24, 1.00, 1.00, 0.00)]:
        key_scale(body, walk, frame, (sx, sx, sz))
        key_loc(body, walk, frame, (0, 0, dz))
    for frame, p in [(1, 0), (7, 10), (15, -12), (20, 4), (24, 0)]:
        key_rot(top, walk, frame, (p, 0, 0))

    # punch: a goopy forward lunge that stretches toward the target then snaps back.
    punch = bpy.data.actions.new("punch")
    for frame, sy, sx, dy, dz in [(1, 1.00, 1.00, 0.0, 0.0), (5, 0.92, 1.08, -0.05, 0.0),
                                  (9, 1.30, 0.84, 0.20, 0.04), (13, 1.05, 0.97, 0.05, 0.0),
                                  (18, 1.00, 1.00, 0.0, 0.0)]:
        key_scale(body, punch, frame, (sx, sy, sx))
        key_loc(body, punch, frame, (0, dy, dz))
    for frame, p in [(1, 0), (5, -8), (9, 20), (18, 0)]:
        key_rot(top, punch, frame, (p, 0, 0))

    # --- detach live actions, stash each on its own NLA track so all export ---
    for obj in (body, top):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(body, idle); stash(top, idle)
    stash(body, walk); stash(top, walk)
    stash(body, punch); stash(top, punch)

    # neutral rest pose so the exporter doesn't bake a keyed pose as rest
    body.location = Vector((0, 0, 0)); body.scale = Vector((1, 1, 1)); body.rotation_euler = (0, 0, 0)
    top.location = Vector((0, 0, TOP_Z)); top.scale = Vector((1, 1, 1)); top.rotation_euler = (0, 0, 0)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "slime.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_slime] wrote", out_path)


if __name__ == "__main__":
    main()
