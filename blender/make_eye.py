"""
Build a floating EYE enemy (a beholder-like flier: a big smooth eyeball with a domed
cornea, a colored iris ring, a dark vertical slit pupil, an eyelid rim, red veins and a
fan of waving tentacles) and export assets/models/eye.glb.

KEY ENGINE FACTS (see docs/blender-model-scripting.md):
  * No GPU skinning. Each glTF *primitive* is a rigid "part" pinned to the NODE that holds
    the mesh, drawn at placement * partNodeWorld. So the rig is a hierarchy of EMPTIES and
    meshes are rigidly parented to those bones.
  * A glTF primitive == one (mesh, material) pair. So ALL same-material geometry parented to
    one bone collapses into ONE primitive == ONE draw call, no matter how many verts. This is
    how we add veins / suckers / detail cheaply: assemble() bakes transforms and joins.
  * Clips are routed by name. The game plays "walk" continuously to undulate the tentacles;
    we ALSO add a looping "idle" (slow hover bob + gentle tentacle curl + the eyeball drifting
    its gaze) so the creature is alive when standing still.

RIG (front faces +Y, the iris/pupil; tentacles fan out the back and trail -Y):
  eye(root) -> eyeball (the gaze bone: glances around in idle without moving the tentacles)
  eye(root) -> tentN_0 -> tentN_1 -> tentN_2   (3 bones per tentacle so they undulate softly,
                                                instead of swinging as one rigid stick)
The eyelid rim sits on the root so it stays put while the eyeball rolls behind it.

DESIGN: high-subdivision icospheres + tapered cone tentacles + SMOOTH shading -> organic,
rounded silhouette. Per-bone same-material joins keep the whole thing to ~24 draw calls.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_eye.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

WHITE  = (0.90, 0.90, 0.92, 1.0)   # sclera
CORNEA = (0.55, 0.74, 0.86, 0.6)   # glassy dome over the iris
IRIS   = (0.85, 0.14, 0.10, 1.0)   # angry, glowing red iris ring
PUPIL  = (0.02, 0.02, 0.03, 1.0)   # dark vertical slit
VEIN   = (0.70, 0.07, 0.07, 1.0)   # bloodshot veins
LID    = (0.34, 0.07, 0.07, 1.0)   # fleshy eyelid rim
TENT   = (0.62, 0.06, 0.06, 1.0)   # tentacles + suckers (one material -> one draw call)


def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass


def mat(name, rgba, emissive=False, strength=2.5):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    if b:
        b.inputs["Base Color"].default_value = rgba
        if emissive and "Emission Color" in b.inputs:
            b.inputs["Emission Color"].default_value = rgba
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = strength
    return m


def make_empty(name, parent, loc, rot_deg=(0, 0, 0)):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = 'ARROWS'; e.empty_display_size = 0.08
    bpy.context.collection.objects.link(e)
    if parent:
        e.parent = parent; e.matrix_parent_inverse = Matrix.Identity(4)
    e.location = Vector(loc)
    e.rotation_euler = tuple(math.radians(a) for a in rot_deg)
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

def cone(name, r_bot, r_top, depth, loc, mat_, rot=None, segs=12):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs,
                          radius1=r_bot, radius2=r_top, depth=depth)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), True)

def torus(name, major, minor, loc, mat_, rot=None, mseg=28, minseg=10):
    bpy.ops.mesh.primitive_torus_add(major_radius=major, minor_radius=minor,
                                     major_segments=mseg, minor_segments=minseg,
                                     location=(0, 0, 0))
    o = bpy.context.active_object; o.name = name
    for p in o.data.polygons: p.use_smooth = True
    o.location = Vector(loc)
    if rot: o.rotation_euler = tuple(math.radians(a) for a in rot)
    o.data.materials.append(mat_)
    return o


def assemble(name, bone, objs):
    """Bake transforms and JOIN -> one mesh object holding one primitive per material,
    rigidly parented to the (mesh-less) bone empty."""
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
    """Absolute orientation keyframe. Clips are full TRS overrides, so each clip defines the
    bone's pose completely (incl. the tentacle's rest tilt) and never relies on rest state."""
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


NT, NS = 6, 3   # 6 tentacles, 3 bones each


def main():
    reset_scene()
    sclera = mat("e_sclera", WHITE)
    cornea = mat("e_cornea", CORNEA)
    iris   = mat("e_iris", IRIS, emissive=True, strength=3.0)
    pupil  = mat("e_pupil", PUPIL)
    vein   = mat("e_vein", VEIN)
    lid    = mat("e_lid", LID)
    tent   = mat("e_tent", TENT)

    # --- Rig ---
    eye     = make_empty("eye", None, (0, 0, 0.0))       # root
    eyeball = make_empty("eyeball", eye, (0, 0, 0.0))    # gaze bone (new): rolls in idle

    # --- Eyeball: sclera + bloodshot veins + iris ring + slit pupil + glassy cornea dome.
    # All joined onto the gaze bone -> 5 primitives (one per material). ---
    eparts = [sphere("globe", 0.45, (0, 0, 0), sclera, subdiv=4)]
    # Bloodshot veins: thin cones creeping front-to-back over the sclera (one joined prim).
    for i, ang in enumerate((15, 65, 120, 170, 205, 255, 300, 345)):
        a = math.radians(ang)
        x, z = math.cos(a) * 0.40, math.sin(a) * 0.40
        bend = 8 + (i % 3) * 5
        eparts.append(cone("vein%d" % i, 0.010, 0.004, 0.66, (x, -0.04, z), vein,
                           rot=(90, 0, bend), segs=6))
    # Iris ring: flattened disc pressed against the +Y face.
    eparts.append(sphere("iris", 0.205, (0, 0.40, 0), iris, scale=(1.0, 0.22, 1.0), subdiv=3))
    # Slit pupil: tall thin dark slab in the iris centre.
    eparts.append(sphere("pupil", 0.12, (0, 0.455, 0), pupil, scale=(0.26, 0.30, 1.05), subdiv=2))
    # Cornea: glassy dome bulging out over the iris.
    eparts.append(sphere("cornea", 0.30, (0, 0.30, 0), cornea, scale=(1.0, 0.55, 1.0), subdiv=3))
    assemble("globe", eyeball, eparts)

    # --- Eyelid rim on the root (stays put while the eyeball rolls behind it). ---
    assemble("eyelid", eye, [
        torus("lidrim", 0.30, 0.045, (0, 0.34, 0), lid, rot=(90, 0, 0)),
    ])

    # --- Tentacles: a fan of 3-bone chains trailing the back; tapered cones + suckers
    # (suckers share the tentacle material, so each segment is ONE draw call). ---
    seg_objs = []   # (empty, base_euler_deg, t, s)
    for t in range(NT):
        spread = math.radians(-50 + t * (100.0 / (NT - 1)))     # fan across the back
        bx, bz = math.sin(spread) * 0.34, math.cos(spread) * 0.08 - 0.18
        base0 = (-72.0, 0.0, math.degrees(spread * 0.4))        # tilt back & down + fan
        seg = make_empty("tent%d_0" % t, eye, (bx, -0.34, bz), rot_deg=base0)
        chain = [(seg, base0)]
        for s in range(1, NS):
            seg = make_empty("tent%d_%d" % (t, s), chain[-1][0], (0, 0, -0.26))
            chain.append((seg, (0.0, 0.0, 0.0)))
        for s, (e, base) in enumerate(chain):
            r_top = 0.075 - s * 0.018          # thicker toward the eye (+Z)
            r_bot = 0.075 - (s + 1) * 0.018     # thinner toward the tip (-Z)
            parts = [cone("tcone%d_%d" % (t, s), r_bot, r_top, 0.26, (0, 0, -0.13), tent, segs=10)]
            for k, zz in enumerate((-0.05, -0.13, -0.21)):
                rr = (r_top + (r_bot - r_top) * ((-zz) / 0.26))
                parts.append(sphere("suck%d_%d_%d" % (t, s, k), 0.020,
                                    (0, -rr * 0.85, zz), tent, scale=(1.0, 0.5, 1.0), subdiv=1))
            assemble("tseg%d_%d" % (t, s), e, parts)
            seg_objs.append((e, base, t, s))

    # --- walk: a travelling sine wave undulates the tentacles (the game loops this). ---
    walk = bpy.data.actions.new("walk")
    wframes = [1, 7, 13, 19, 25]
    for (e, base, t, s) in seg_objs:
        for f in wframes:
            phase = (f / 24.0) * 2 * math.pi + t * 0.7 + s * 0.9
            amp = 10 + s * 6                                  # tips swing more
            key_rot(e, walk, f, (base[0] + amp * math.sin(phase), base[1], base[2]))

    # --- idle: slow hover bob + the eyeball drifting its gaze + a gentle tentacle curl. ---
    idle = bpy.data.actions.new("idle")
    for f, z in [(1, 0.0), (15, 0.045), (31, 0.065), (46, 0.03), (61, 0.0)]:
        key_loc(eye, idle, f, (0, 0, z))                     # hover bob
    for f, px, yw in [(1, 0, 0), (16, 4, 9), (31, 2, -7), (46, -3, 6), (61, 0, 0)]:
        key_rot(eyeball, idle, f, (px, 0, yw))               # glance around (pitch X / yaw Z)
    iframes = [1, 21, 41, 61]
    for (e, base, t, s) in seg_objs:
        for f in iframes:
            phase = (f / 60.0) * 2 * math.pi + t * 0.9 + s * 0.6
            amp = 5 + s * 4
            curl = 3 + s * 5                                  # gentle inward droop/curl
            key_rot(e, idle, f, (base[0] + amp * math.sin(phase) - curl, base[1], base[2]))

    # --- Stash actions on NLA so the exporter emits BOTH named animations. ---
    animated = [eye, eyeball] + [e for (e, _b, _t, _s) in seg_objs]
    for o in animated:
        if o.animation_data: o.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(eye, idle); stash(eyeball, idle)
    for (e, _b, _t, _s) in seg_objs:
        stash(e, walk); stash(e, idle)

    # --- Reset to a clean REST pose before export (clips fully override, but keep rest sane
    # so a no-clip draw still shows the creature correctly oriented). ---
    eye.location = Vector((0, 0, 0))
    eyeball.rotation_euler = (0, 0, 0)
    for (e, base, t, s) in seg_objs:
        e.rotation_euler = tuple(math.radians(a) for a in base)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "eye.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_eye] wrote", out_path)


if __name__ == "__main__":
    main()
