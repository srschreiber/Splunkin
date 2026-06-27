"""
Procedurally build a SKELETON and export assets/models/skeleton.glb.

This is the highest-traffic model in the game: it's the player avatar AND every friendly
lane mob (tinted) AND the skeleton enemy. So it's worth detailing — but draw calls matter.

KEY ENGINE FACTS (see docs/blender-model-scripting.md):
  * No GPU skinning. Each glTF *primitive* is a rigid "part" pinned to the NODE that holds
    the mesh, drawn at placement * partNodeWorld. So the rig is a hierarchy of EMPTIES and
    meshes are parented to those bones.
  * A glTF primitive == one (mesh, material) pair. So ALL same-material geometry parented to
    one bone collapses into ONE primitive == ONE draw call, no matter how many vertices.
  * Clips are routed by name: idle / walk / punch / block (others ignored). `idle` is the
    looping base layer the engine plays when standing still; walk replaces it when moving.

RIG (more bones than the old box version, for softer motion):
  body(waist) -> chest -> head, armL/armR(-> handL/handR)
  body -> legL(-> kneeL), legR(-> kneeR)
The CHEST bone lets the upper body breathe in idle without lifting the planted feet; the
KNEE bones let the legs bend through the walk instead of swinging as stiff pegs.

DESIGN: rounded primitives (icospheres / tapered cylinders) + SMOOTH shading -> organic
silhouette; everything bone-white on a bone joins into one primitive, so the whole skeleton
is ~12 parts despite vertebrae, joint knobs, ribs and finger nubs.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_skeleton.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

BONE  = (0.86, 0.84, 0.76, 1.0)
DARK  = (0.04, 0.04, 0.05, 1.0)
EMBER = (0.98, 0.40, 0.10, 1.0)
CHEST_Z = 0.42   # chest bone height (so head/arms ride above the waist)


def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass


def mat(name, rgba, emissive=False):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    if b:
        b.inputs["Base Color"].default_value = rgba
        if emissive and "Emission Color" in b.inputs:
            b.inputs["Emission Color"].default_value = rgba
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = 3.0
    return m


def make_empty(name, parent, loc):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = 'ARROWS'; e.empty_display_size = 0.08
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

def bone_cyl(name, r_bot, r_top, depth, loc, mat_, rot=None, segs=12):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs,
                          radius1=r_bot, radius2=r_top, depth=depth)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), True)

def box(name, size, loc, mat_, rot=None, bev=0.012):
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
    """Keyframe a node's LOCAL translation. Must include the bone's base offset (clips
    set the node transform absolutely, so omitting it would snap the bone to the origin)."""
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.location = Vector(loc)
    obj.keyframe_insert(data_path="location", frame=frame)


def main():
    reset_scene()
    bone, dark, ember = mat("bone", BONE), mat("hollow", DARK), mat("ember", EMBER, emissive=True)

    # --- Rig (empties). FRONT faces +Y. ---
    body  = make_empty("body",  None, (0, 0, 0.0))
    chest = make_empty("chest", body, (0, 0, CHEST_Z))
    head  = make_empty("head",  chest, (0, 0, 0.50))
    armL  = make_empty("armL",  chest, (0.26, 0, 0.40))
    armR  = make_empty("armR",  chest, (-0.26, 0, 0.40))
    handL = make_empty("handL", armL, (0, 0, -0.50))
    handR = make_empty("handR", armR, (0, 0, -0.50))
    legL  = make_empty("legL",  body, (0.12, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.12, 0, 0.0))
    kneeL = make_empty("kneeL", legL, (0, 0, -0.55))
    kneeR = make_empty("kneeR", legR, (0, 0, -0.55))

    # --- SKULL (head bone): rounded cranium + face mass, brow, jaw, cheekbones; sockets + eyes ---
    assemble("skull", head, [
        sphere("cranium", 0.155, (0, -0.01, 0.21), bone, scale=(1.0, 1.08, 1.06), subdiv=3),
        sphere("face",    0.12,  (0, 0.10, 0.14),  bone, scale=(1.0, 0.85, 1.05), subdiv=2),
        box("brow",  (0.22, 0.05, 0.035), (0, 0.135, 0.245), bone, bev=0.01),
        box("jaw",   (0.20, 0.16, 0.06),  (0, 0.085, 0.02),  bone, bev=0.02),
        sphere("cheekL", 0.045, (0.10, 0.11, 0.13), bone, subdiv=2),
        sphere("cheekR", 0.045, (-0.10, 0.11, 0.13), bone, subdiv=2),
        bone_cyl("neck", 0.05, 0.06, 0.14, (0, -0.02, -0.04), bone, segs=10),
    ])
    assemble("sockets", head, [
        sphere("socketL", 0.058, (0.07, 0.125, 0.205), dark, scale=(1.0, 0.7, 1.0), subdiv=2),
        sphere("socketR", 0.058, (-0.07, 0.125, 0.205), dark, scale=(1.0, 0.7, 1.0), subdiv=2),
        sphere("naso",    0.03,  (0, 0.16, 0.135), dark, scale=(0.6, 1.0, 1.3), subdiv=1),
    ])
    assemble("eyes", head, [
        sphere("eyeL", 0.028, (0.07, 0.15, 0.205), ember, subdiv=1),
        sphere("eyeR", 0.028, (-0.07, 0.15, 0.205), ember, subdiv=1),
    ])

    # --- CHEST: ribcage, sternum, collarbones, shoulder blades (all ride the chest bone so
    # they breathe in idle). Local Z is offset by -CHEST_Z from the old absolute heights. ---
    cz = lambda z: z - CHEST_Z
    chest_parts = []
    for i in range(6):                       # thoracic vertebrae
        z = 0.40 + i * 0.085
        chest_parts.append(sphere("tvert%d" % i, 0.04 - i * 0.001, (0, -0.05, cz(z)), bone, scale=(1.1, 1.0, 0.8), subdiv=1))
    chest_parts.append(box("sternum", (0.07, 0.04, 0.34), (0, 0.10, cz(0.52)), bone, bev=0.015))
    for i, z in enumerate((0.40, 0.50, 0.60, 0.69)):
        w = 0.34 - i * 0.03; tilt = -18 - i * 4
        chest_parts.append(box("ribL%d" % i, (w, 0.10, 0.028), (0, 0.04, cz(z)), bone, rot=(0, tilt, 12), bev=0.01))
        chest_parts.append(box("ribR%d" % i, (w, 0.10, 0.028), (0, 0.04, cz(z)), bone, rot=(0, -tilt, -12), bev=0.01))
    chest_parts.append(bone_cyl("clavL", 0.022, 0.022, 0.30, (0.13, 0.07, cz(0.78)), bone, rot=(0, 90, 0), segs=8))
    chest_parts.append(bone_cyl("clavR", 0.022, 0.022, 0.30, (-0.13, 0.07, cz(0.78)), bone, rot=(0, 90, 0), segs=8))
    chest_parts.append(box("scapL", (0.13, 0.03, 0.18), (0.12, -0.08, cz(0.66)), bone, rot=(0, 14, 0), bev=0.015))
    chest_parts.append(box("scapR", (0.13, 0.03, 0.18), (-0.12, -0.08, cz(0.66)), bone, rot=(0, -14, 0), bev=0.015))
    assemble("ribcage", chest, chest_parts)

    # --- PELVIS stays on the waist (body) so it doesn't breathe with the chest. ---
    assemble("pelvis", body, [
        box("sacrum", (0.16, 0.14, 0.14), (0, -0.01, 0.06), bone, bev=0.03),
        box("iliacL", (0.10, 0.07, 0.20), (0.13, 0.02, 0.10), bone, rot=(0, 18, 0), bev=0.02),
        box("iliacR", (0.10, 0.07, 0.20), (-0.13, 0.02, 0.10), bone, rot=(0, -18, 0), bev=0.02),
        # lumbar verts bridging up to the chest
        sphere("lvert0", 0.04, (0, -0.05, 0.22), bone, scale=(1.1, 1.0, 0.8), subdiv=1),
        sphere("lvert1", 0.04, (0, -0.05, 0.31), bone, scale=(1.1, 1.0, 0.8), subdiv=1),
    ])

    # --- ARMS: shoulder knob + tapered humerus on the arm bone. ---
    assemble("upperL", armL, [
        sphere("shoulderL", 0.07, (0, 0, 0.0), bone, subdiv=2),
        bone_cyl("humerusL", 0.034, 0.05, 0.50, (0, 0, -0.26), bone, segs=10),
    ])
    assemble("upperR", armR, [
        sphere("shoulderR", 0.07, (0, 0, 0.0), bone, subdiv=2),
        bone_cyl("humerusR", 0.034, 0.05, 0.50, (0, 0, -0.26), bone, segs=10),
    ])

    # --- FOREARMS + HANDS: elbow knob, radius, wrist, palm + finger nubs. ---
    def forearm(side, bonenode):
        parts = [
            sphere("elbow%s" % side, 0.05, (0, 0, 0.02), bone, subdiv=2),
            bone_cyl("radius%s" % side, 0.028, 0.04, 0.30, (0, 0, -0.14), bone, segs=10),
            sphere("wrist%s" % side, 0.04, (0, 0, -0.30), bone, subdiv=1),
            box("palm%s" % side, (0.085, 0.05, 0.09), (0, 0, -0.37), bone, bev=0.02),
        ]
        for k in range(4):
            x = -0.03 + k * 0.02
            parts.append(bone_cyl("fing%s%d" % (side, k), 0.012, 0.014, 0.09, (x, 0, -0.45), bone, segs=6))
        parts.append(bone_cyl("thumb%s" % side, 0.014, 0.016, 0.06, (0.05, 0.0, -0.39), bone, rot=(0, 55, 0), segs=6))
        assemble("hand%s" % side, bonenode, parts)
    forearm("L", handL); forearm("R", handR)

    # --- LEGS: thigh on the leg bone, shin+foot on the knee bone (so the knee bends). ---
    def thigh(side, bonenode):
        assemble("thigh%s" % side, bonenode, [
            sphere("hip%s" % side, 0.07, (0, 0, 0.02), bone, subdiv=2),
            bone_cyl("femur%s" % side, 0.045, 0.06, 0.52, (0, 0, -0.27), bone, segs=10),
            sphere("knee%s" % side, 0.055, (0, 0, -0.55), bone, subdiv=2),
        ])
    def shin(side, bonenode):
        assemble("shin%s" % side, bonenode, [
            bone_cyl("tibia%s" % side, 0.032, 0.05, 0.46, (0, 0, -0.25), bone, segs=10),
            box("foot%s" % side, (0.10, 0.22, 0.07), (0, 0.06, -0.51), bone, bev=0.02),
        ])
    thigh("L", legL); shin("L", kneeL)
    thigh("R", legR); shin("R", kneeR)

    # --- Animations ---
    # walk: thighs + arms swing opposite; knees bend on the back-swing for a real stride.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 25), (13, -25), (25, 25)]:
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a * 0.7, 0, 0))
        key_rot(armR, walk, frame, (a * 0.7, 0, 0))
    # knees flex when their thigh is swinging back (negative thigh angle), straighten forward.
    for frame, kl, kr in [(1, 8, 45), (7, 35, 25), (13, 45, 8), (19, 25, 35), (25, 8, 45)]:
        key_rot(kneeL, walk, frame, (kl, 0, 0))
        key_rot(kneeR, walk, frame, (kr, 0, 0))

    # idle: slow breathing. The chest lifts/leans + arms sway + head bobs while feet stay planted.
    idle = bpy.data.actions.new("idle")
    for frame, rise, lean in [(1, 0.0, 0.0), (31, 0.022, 2.0), (61, 0.0, 0.0)]:
        key_loc(chest, idle, frame, (0, 0, CHEST_Z + rise))
        key_rot(chest, idle, frame, (lean, 0, 0))
    for frame, a in [(1, 0), (31, -2.5), (61, 0)]:
        key_rot(head, idle, frame, (a, 0, 0))
    for frame, ax, az in [(1, 3, 4), (31, 6, 7), (61, 3, 4)]:
        key_rot(armL, idle, frame, (ax, 0, az))
        key_rot(armR, idle, frame, (ax, 0, -az))

    # punch: armL winds back then swings forward/down (wrist follows through).
    punch = bpy.data.actions.new("punch")
    for frame, a in [(1, 0), (5, -40), (10, 100), (16, 0)]:
        key_rot(armL, punch, frame, (a, 0, 0))
    key_rot(handL, punch, 10, (25, 0, 0))
    key_rot(handL, punch, 16, (0, 0, 0))

    for obj in (chest, head, legL, legR, armL, armR, handL, kneeL, kneeR):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk); stash(armR, walk); stash(armL, walk)
    stash(kneeL, walk); stash(kneeR, walk)
    stash(chest, idle); stash(head, idle); stash(armL, idle); stash(armR, idle)
    stash(armL, punch); stash(handL, punch)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "skeleton.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_skeleton] wrote", out_path)


if __name__ == "__main__":
    main()
