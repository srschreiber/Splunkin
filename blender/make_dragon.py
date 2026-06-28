"""
Build a big menacing DRAGON (top-tier enemy) and export assets/models/dragon.glb.

Behavior (engine): a scaly winged quadruped beast whose attack is a sweeping FIRE BREATH.
The engine rotates the `head` bone left->right and emits fire from the mouth, so the head is
its OWN animatable bone with the muzzle/jaw at the front (+Y). See docs/blender-model-scripting.md.

KEY ENGINE FACTS (mirrored from make_skeleton.py / make_demon.py, the gold standards):
  * No GPU skinning. Each glTF *primitive* is a rigid "part" pinned to the NODE that holds the
    mesh, drawn at placement * partNodeWorld. So the rig is a hierarchy of EMPTIES and meshes
    are parented to those bones.
  * A glTF primitive == one (mesh, material) pair. So ALL same-material geometry parented to one
    bone collapses into ONE primitive == ONE draw call -> add scales/spikes/struts freely as long
    as they share a material; mixing N materials on one bone costs N prims.
  * Clips routed by name: idle / walk / punch (others ignored). `idle` loops when the beast holds
    station; walk replaces it when moving; punch is the fire-breath wind-up.

ORIENTATION / SCALE (NOT the waist-origin character convention):
  * Z up (exporter -> Y-up). FACES +Y (muzzle toward +Y), like the other char models.
  * Origin at the CENTER of the footprint at GROUND level: feet at z~=0, body rises in +Z (like a
    ground prop, not the feet-at-Z=-1 humanoids). It's BIG: ~3 units long, ~1.8 at the shoulder,
    a ~4-unit wingspan.

RIG (mesh-less empties; these names matter to the engine):
  body(torso root) -> head(long arched neck + horned skull + jaws) -> jaw(gaping lower jaw)
  body -> wingL/wingR(-> wingtipL/wingtipR), tail, legFL/legFR/legBL/legBR (four legs)
The HEAD is its own bone so the engine can sweep it for the breath; the MUZZLE TIP sits at the
front of the head bone so fire spawns there.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_dragon.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

SCALE_C = (0.30, 0.07, 0.06, 1.0)   # dark-red scaly hide
DARK    = (0.10, 0.09, 0.11, 1.0)   # charcoal belly / horns / spikes / claws / struts
EMBER   = (1.00, 0.45, 0.10, 1.0)   # emissive throat / eyes / maw
WING    = (0.20, 0.06, 0.07, 1.0)   # membrane
TOOTH   = (0.86, 0.83, 0.72, 1.0)   # fangs

BODY_Z = 1.05   # torso (root) bone height above the ground


def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass


def mat(name, rgba, emissive=False, strength=3.5):
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

def bone_cyl(name, r_bot, r_top, depth, loc, mat_, rot=None, segs=12):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs,
                          radius1=r_bot, radius2=r_top, depth=depth)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), True)

def box(name, size, loc, mat_, rot=None, bev=0.012, scale=(1, 1, 1)):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, scale, False)


def assemble(name, bone, objs):
    """Bake transforms, JOIN same-material detail into one mesh, pin rigidly to the bone."""
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


def main():
    reset_scene()
    scl  = mat("d_scale", SCALE_C)
    dark = mat("d_dark",  DARK)
    emb  = mat("d_ember", EMBER, emissive=True)
    wing = mat("d_wing",  WING)
    tth  = mat("d_tooth", TOOTH)

    # ============================ RIG (empties). FRONT faces +Y. ============================
    body = make_empty("body", None, (0, 0, BODY_Z))           # torso root, at shoulder height
    head = make_empty("head", body, (0, 0.95, 0.70))          # base of skull (top of the arched neck)
    jaw  = make_empty("jaw",  head, (0, 0.18, -0.08))         # lower-jaw hinge near the back of the mouth
    tail = make_empty("tail", body, (0, -0.70, -0.05))
    wingL = make_empty("wingL", body, (0.30, -0.05, 0.45))    # roots high on the back
    wingR = make_empty("wingR", body, (-0.30, -0.05, 0.45))
    wtipL = make_empty("wingtipL", wingL, (0.95, -0.10, 0.05))
    wtipR = make_empty("wingtipR", wingR, (-0.95, -0.10, 0.05))
    legFL = make_empty("legFL", body, (0.42, 0.50, -0.10))    # world z 0.95
    legFR = make_empty("legFR", body, (-0.42, 0.50, -0.10))
    legBL = make_empty("legBL", body, (0.46, -0.55, -0.05))   # world z 1.00
    legBR = make_empty("legBR", body, (-0.46, -0.55, -0.05))

    # ================================ TORSO (body bone) ================================
    # Barrel chest + belly + back ridge of spikes. Scaly hide (1 prim) + charcoal belly/spikes (1 prim).
    assemble("torso", body, [
        sphere("chest",   0.55, (0, 0.42, 0.06),  scl, scale=(1.0, 1.05, 0.95), subdiv=3),
        sphere("barrel",  0.60, (0, -0.10, 0.0),  scl, scale=(1.05, 1.25, 1.0), subdiv=3),
        sphere("rump",    0.48, (0, -0.62, 0.02), scl, scale=(1.0, 1.0, 0.95), subdiv=2),
        sphere("shoulderL", 0.24, (0.40, 0.46, 0.05), scl, subdiv=2),
        sphere("shoulderR", 0.24, (-0.40, 0.46, 0.05), scl, subdiv=2),
        sphere("haunchL",  0.30, (0.43, -0.56, 0.0), scl, subdiv=2),
        sphere("haunchR",  0.30, (-0.43, -0.56, 0.0), scl, subdiv=2),
    ])
    belly = [
        sphere("belly", 0.46, (0, -0.05, -0.28), dark, scale=(1.0, 1.35, 0.7), subdiv=2),
        sphere("brisket", 0.30, (0, 0.45, -0.22), dark, scale=(1.0, 1.0, 0.8), subdiv=2),
    ]
    # dorsal spike ridge running the spine (neck base -> rump), peaking over the shoulders
    for i, (y, h) in enumerate([(0.55, 0.16), (0.30, 0.22), (0.05, 0.26), (-0.22, 0.24),
                                (-0.48, 0.19), (-0.70, 0.14)]):
        belly.append(bone_cyl("dspk%d" % i, 0.05, 0.0, h, (0, y, 0.52), dark, rot=(18, 0, 0), segs=6))
    assemble("underside", body, belly)

    # ================================ HEAD (head bone) ================================
    # Long arched neck spheres bridging down-back to the chest, a horned skull, a forward muzzle
    # (the MUZZLE TIP is where fire spawns), heavy brow, glowing eyes, upper fangs, ember throat.
    assemble("skull", head, [
        sphere("neck0", 0.27, (0, -0.55, -0.55), scl, subdiv=2),
        sphere("neck1", 0.25, (0, -0.34, -0.32), scl, subdiv=2),
        sphere("neck2", 0.23, (0, -0.12, -0.10), scl, subdiv=2),
        sphere("cranium", 0.23, (0, 0.17, 0.06), scl, scale=(0.95, 1.05, 0.95), subdiv=3),
        sphere("muzzle", 0.17, (0, 0.46, 0.0), scl, scale=(0.85, 1.55, 0.72), subdiv=2),
        box("snoutbridge", (0.18, 0.34, 0.10), (0, 0.40, 0.06), scl, rot=(4, 0, 0), bev=0.03),
        sphere("browmass", 0.16, (0, 0.26, 0.18), scl, scale=(1.4, 0.7, 0.7), subdiv=2),
        sphere("jowlL", 0.10, (0.16, 0.20, -0.05), scl, subdiv=2),
        sphere("jowlR", 0.10, (-0.16, 0.20, -0.05), scl, subdiv=2),
    ])
    # horns + cheek/jaw frill spikes + brow ridge plate (charcoal, 1 prim)
    horns = [
        bone_cyl("hornL0", 0.06, 0.035, 0.30, (0.14, -0.06, 0.30), dark, rot=(40, 0, 14), segs=10),
        bone_cyl("hornL1", 0.035, 0.0, 0.28, (0.20, -0.26, 0.44), dark, rot=(68, 0, 20), segs=10),
        bone_cyl("hornR0", 0.06, 0.035, 0.30, (-0.14, -0.06, 0.30), dark, rot=(40, 0, -14), segs=10),
        bone_cyl("hornR1", 0.035, 0.0, 0.28, (-0.20, -0.26, 0.44), dark, rot=(68, 0, -20), segs=10),
        box("browridge", (0.34, 0.10, 0.06), (0, 0.30, 0.20), dark, rot=(-16, 0, 0), bev=0.02),
    ]
    for k, (x, s) in enumerate([(0.20, 1), (0.16, 1), (-0.20, 1), (-0.16, 1)]):
        horns.append(bone_cyl("frill%d" % k, 0.03, 0.0, 0.16, (x, 0.02 - 0.14 * (k % 2), 0.02),
                              dark, rot=(70, 0, 50 * (1 if x > 0 else -1)), segs=6))
    assemble("horns", head, horns)
    assemble("eyes", head, [
        sphere("eyeL", 0.05, (0.15, 0.30, 0.13), emb, scale=(1.1, 0.8, 0.7), subdiv=2),
        sphere("eyeR", 0.05, (-0.15, 0.30, 0.13), emb, scale=(1.1, 0.8, 0.7), subdiv=2),
        sphere("throat", 0.13, (0, -0.10, -0.34), emb, scale=(1.0, 0.9, 1.1), subdiv=2),  # glowing throat
    ])
    upper_teeth = []
    for k, x in enumerate((-0.10, -0.035, 0.035, 0.10)):
        upper_teeth.append(bone_cyl("utooth%d" % k, 0.022, 0.0, 0.10, (x, 0.52, -0.10),
                                    tth, rot=(180, 0, 0), segs=6))
    assemble("upperteeth", head, upper_teeth)

    # ================================ JAW (jaw bone, gapes) ================================
    jaw_parts = [
        box("jawmesh", (0.26, 0.40, 0.11), (0, 0.34, -0.07), scl, rot=(-3, 0, 0), bev=0.03),
        sphere("chin", 0.10, (0, 0.54, -0.08), scl, subdiv=2),
    ]
    assemble("jawmass", jaw, jaw_parts)
    lower_teeth = []
    for k, x in enumerate((-0.10, -0.035, 0.035, 0.10)):
        lower_teeth.append(bone_cyl("ltooth%d" % k, 0.020, 0.0, 0.09, (x, 0.50, 0.0), tth, segs=6))
    assemble("lowerteeth", jaw, lower_teeth)
    assemble("maw", jaw, [
        sphere("mouthglow", 0.12, (0, 0.30, -0.02), emb, scale=(1.1, 1.3, 0.6), subdiv=2),
    ])

    # ================================ LEGS (four clawed legs) ================================
    def leg(name, bonenode, topz, front):
        fz = -topz                     # foot sole reaches the ground (world z 0)
        thy = 0.06 if front else 0.10  # back legs tuck slightly forward
        parts = [
            sphere("hip", 0.20, (0, 0, 0.0), scl, subdiv=2),
            bone_cyl("thigh", 0.17, 0.11, topz * 0.46, (0, thy, -topz * 0.26), scl, segs=10),
            sphere("knee", 0.12, (0, thy * 0.4, -topz * 0.52), scl, subdiv=2),
            bone_cyl("shin", 0.10, 0.075, topz * 0.42, (0, 0.04, -topz * 0.74), scl, segs=10),
            box("foot", (0.24, 0.36, 0.11), (0, 0.13, fz + 0.06), scl, bev=0.02),
        ]
        assemble(name, bonenode, parts)
        claws = []
        for k, x in enumerate((-0.085, 0.0, 0.085)):
            claws.append(bone_cyl("claw%d" % k, 0.032, 0.0, 0.14, (x, 0.30, fz + 0.05),
                                  dark, rot=(72, 0, 0), segs=6))
        assemble(name + "claw", bonenode, claws)
    leg("legFL", legFL, 0.95, True)
    leg("legFR", legFR, 0.95, True)
    leg("legBL", legBL, 1.00, False)
    leg("legBR", legBR, 1.00, False)

    # ================================ TAIL (tapering spiked tail) ================================
    tail_scale = [
        sphere("t0", 0.30, (0, -0.18, 0.02), scl, subdiv=2),
        sphere("t1", 0.24, (0, -0.52, -0.10), scl, subdiv=2),
        sphere("t2", 0.18, (0, -0.86, -0.26), scl, subdiv=2),
        sphere("t3", 0.13, (0, -1.16, -0.42), scl, subdiv=2),
        sphere("t4", 0.08, (0, -1.42, -0.54), scl, subdiv=2),
    ]
    assemble("tail", tail, tail_scale)
    tail_dark = []
    for i, (y, z, h) in enumerate([(-0.30, 0.22, 0.16), (-0.62, 0.05, 0.14),
                                   (-0.94, -0.12, 0.11), (-1.20, -0.28, 0.09)]):
        tail_dark.append(bone_cyl("tspk%d" % i, 0.04, 0.0, h, (0, y, z), dark, rot=(20, 0, 0), segs=6))
    # tail-tip spade
    tail_dark.append(box("spadeL", (0.05, 0.26, 0.14), (0.07, -1.55, -0.56), dark, rot=(0, 0, 24), bev=0.02))
    tail_dark.append(box("spadeR", (0.05, 0.26, 0.14), (-0.07, -1.55, -0.56), dark, rot=(0, 0, -24), bev=0.02))
    assemble("tailspikes", tail, tail_dark)

    # ================================ WINGS (membrane + struts) ================================
    def wing_inner(side, bonenode):
        s = 1.0 if side == "L" else -1.0
        assemble("wingmemb%s" % side, bonenode, [
            box("memb%s" % side, (0.92, 0.86, 0.025), (s * 0.50, -0.16, 0.0), wing,
                rot=(0, 0, -16 * s), bev=0.02),
        ])
        assemble("wingrib%s" % side, bonenode, [
            bone_cyl("lead%s" % side, 0.06, 0.035, 1.0, (s * 0.48, 0.10, 0.08), dark, rot=(0, 90 * s, 0), segs=8),
            bone_cyl("ribA%s" % side, 0.032, 0.012, 0.70, (s * 0.40, -0.28, 0.0), dark, rot=(0, 70 * s, 0), segs=6),
            bone_cyl("ribB%s" % side, 0.030, 0.012, 0.62, (s * 0.28, -0.50, -0.02), dark, rot=(0, 52 * s, 0), segs=6),
        ])
    def wing_outer(side, bonenode):
        s = 1.0 if side == "L" else -1.0
        assemble("tipmemb%s" % side, bonenode, [
            box("tipmemb%s" % side, (0.70, 0.74, 0.022), (s * 0.28, -0.24, -0.02), wing,
                rot=(0, 0, -26 * s), bev=0.02),
        ])
        assemble("tiprib%s" % side, bonenode, [
            bone_cyl("tipbone%s" % side, 0.034, 0.0, 0.66, (s * 0.32, 0.02, 0.04), dark, rot=(0, 80 * s, 0), segs=6),
            bone_cyl("tipfing%s" % side, 0.026, 0.01, 0.52, (s * 0.18, -0.40, -0.05), dark, rot=(0, 44 * s, 0), segs=6),
            bone_cyl("tipclaw%s" % side, 0.03, 0.0, 0.14, (s * 0.62, 0.10, 0.10), dark, rot=(0, 110 * s, 0), segs=6),
        ])
    wing_inner("L", wingL); wing_outer("L", wtipL)
    wing_inner("R", wingR); wing_outer("R", wtipR)

    # =============================== ANIMATIONS ===============================
    # idle: slow breathing (body rises, chest swells), head/neck bob, wings settle, tail sway, jaw breath.
    idle = bpy.data.actions.new("idle")
    for frame, z in [(1, 0.0), (31, 0.04), (61, 0.0)]:
        key_loc(body, idle, frame, (0, 0, BODY_Z + z))
    for frame, a, y in [(1, 2, 4), (31, -3, -4), (61, 2, 4)]:
        key_rot(head, idle, frame, (a, 0, y))
    for frame, j in [(1, 0), (31, 6), (61, 0)]:
        key_rot(jaw, idle, frame, (j, 0, 0))
    for frame, w in [(1, 14), (31, 3), (61, 14)]:            # wings settle/breathe (about Y)
        key_rot(wingL, idle, frame, (0, w, 4))
        key_rot(wingR, idle, frame, (0, -w, -4))
    for frame, w in [(1, -6), (21, 10), (41, -6), (61, -6)]: # tips whip with a lag
        key_rot(wtipL, idle, frame, (0, w, 0))
        key_rot(wtipR, idle, frame, (0, -w, 0))
    for frame, a in [(1, 9), (31, -9), (61, 9)]:             # tail sway (about Z)
        key_rot(tail, idle, frame, (0, 0, a))

    # walk: heavy four-legged lumber. Diagonal gait (FL+BR vs FR+BL), body bob+roll, neck bob,
    # wings half-flap, tail sway.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 22), (13, -22), (25, 22)]:
        key_rot(legFL, walk, frame, (a, 0, 0)); key_rot(legBR, walk, frame, (a, 0, 0))
        key_rot(legFR, walk, frame, (-a, 0, 0)); key_rot(legBL, walk, frame, (-a, 0, 0))
    for frame, z in [(1, 0.0), (7, 0.05), (13, 0.0), (19, 0.05), (25, 0.0)]:
        key_loc(body, walk, frame, (0, 0, BODY_Z + z))
    for frame, r in [(1, 0), (7, 4), (13, 0), (19, -4), (25, 0)]:
        key_rot(body, walk, frame, (0, r, 0))
    for frame, a in [(1, 0), (13, 5), (25, 0)]:
        key_rot(head, walk, frame, (a, 0, 0))
    for frame, w in [(1, 20), (13, -8), (25, 20)]:           # wings half-flap
        key_rot(wingL, walk, frame, (0, w, 4)); key_rot(wingR, walk, frame, (0, -w, -4))
    for frame, a in [(1, 12), (13, -12), (25, 12)]:          # tail sway
        key_rot(tail, walk, frame, (0, 0, a))

    # punch: FIRE-BREATH wind-up. Rears the head/neck UP and BACK, jaw gapes wide, wings flare,
    # then settles. (The engine drives the left->right sweep + fire particles on top of this.)
    punch = bpy.data.actions.new("punch")
    for frame, a in [(1, 0), (8, -38), (16, -30), (24, 0)]:  # rear head up/back
        key_rot(head, punch, frame, (a, 0, 0))
    for frame, j in [(1, 0), (8, 18), (14, 56), (20, 50), (24, 6)]:  # jaw gapes wide
        key_rot(jaw, punch, frame, (j, 0, 0))
    for frame, r in [(1, 0), (8, -7), (16, -5), (24, 0)]:    # body rears slightly
        key_rot(body, punch, frame, (r, 0, 0))
    for frame, w in [(1, 14), (8, 46), (16, 36), (24, 14)]:  # wings flare open
        key_rot(wingL, punch, frame, (0, w, 4)); key_rot(wingR, punch, frame, (0, -w, -4))

    # ---- Reset every animated bone to neutral, then stash each action on its own NLA track ----
    animated = (body, head, jaw, tail, wingL, wingR, wtipL, wtipR, legFL, legFR, legBL, legBR)
    for obj in animated:
        if obj.animation_data: obj.animation_data.action = None
        obj.rotation_euler = (0, 0, 0)
    body.location = Vector((0, 0, BODY_Z))

    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)

    for obj in (body, head, jaw, tail, wingL, wingR, wtipL, wtipR):
        stash(obj, idle)
    for obj in (body, head, tail, wingL, wingR, legFL, legFR, legBL, legBR):
        stash(obj, walk)
    for obj in (head, jaw, body, wingL, wingR):
        stash(obj, punch)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "dragon.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_dragon] wrote", out_path)


if __name__ == "__main__":
    main()
