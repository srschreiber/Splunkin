"""
Build a TROLL enemy (big, hunched, club-swinging brute) and export assets/models/troll.glb.
Behavior (in the engine): slow, huge HP, a big TELEGRAPHED overhead club SLAM with massive
damage + knockback. See docs/blender-model-scripting.md.

KEY ENGINE FACTS (mirrors make_skeleton.py, the gold-standard):
  * No GPU skinning. Each glTF *primitive* is a rigid "part" pinned to the NODE that holds the
    mesh, drawn at placement * partNodeWorld. So the rig is a hierarchy of EMPTIES and meshes
    are rigidly parented to those bones.
  * A glTF primitive == one (mesh, material) pair. ALL same-material geometry joined onto one
    bone collapses into ONE primitive == ONE draw call, no matter the vertex count -> we can
    pile on brow, knuckles, hump, belly, teeth for free as long as we join per material.
  * Clips routed by name: idle / walk / punch (others ignored). `idle` is the looping base
    layer when standing; walk replaces it moving; the engine masks `punch` to armL + subtree
    and samples it DURING the wind-up, so the CLUB lives in the LEFT hand.

RIG (more bones than the old box version, for softer / heavier motion):
  body(waist) -> chest -> head -> jaw, armL/armR(-> handL/handR)
  body -> legL(-> kneeL), legR(-> kneeR)
The CHEST bone lets the huge upper body HEAVE when breathing without lifting the planted feet;
the JAW drops on each breath; the KNEE bones let the stubby legs bend through the lumber.

DESIGN: rounded SMOOTH-shaded icospheres + tapered cylinders read as slabs of muscle and fat,
not crates. Everything sharing a material on a bone joins into one primitive, so the whole
hulk is ~23 parts despite pecs, hump, gut, brow, tusks, knuckles and a spiked stone club.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_troll.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

SKIN  = (0.40, 0.50, 0.36, 1.0)   # grey-green hide
DARK  = (0.30, 0.38, 0.28, 1.0)   # darker hide (hump, brow, feet)
LOIN  = (0.36, 0.26, 0.16, 1.0)   # leather loincloth
NAIL  = (0.85, 0.84, 0.78, 1.0)   # tusks / teeth / claws
STONE = (0.50, 0.50, 0.54, 1.0)   # club shaft
DSTONE= (0.38, 0.38, 0.42, 1.0)   # club head + spikes
EYE   = (0.95, 0.75, 0.15, 1.0)   # yellow eyes (emissive)

CHEST_LOC = (0.0, 0.04, 0.40)     # chest bone offset from waist (head/arms ride above it)


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
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = 2.5
    return m


def make_empty(name, parent, loc):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = 'ARROWS'; e.empty_display_size = 0.1
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
    """Bake each part's transform into its verts, JOIN all of them (same material) into ONE
    object = ONE glTF primitive = ONE draw call, then rigidly parent the result to `bone`."""
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
    """Keyframe a node's LOCAL translation. Must include the bone's base offset (clips set the
    node transform absolutely, so omitting it would snap the bone to the origin)."""
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.location = Vector(loc)
    obj.keyframe_insert(data_path="location", frame=frame)


def main():
    reset_scene()
    skin, dark, loin, nail, stone, dstone, eye = (
        mat("t_skin", SKIN), mat("t_dark", DARK), mat("t_loin", LOIN), mat("t_nail", NAIL),
        mat("t_stone", STONE), mat("t_dstone", DSTONE), mat("t_eye", EYE, emissive=True))

    # --- Rig (empties). FRONT faces +Y. Hunched: head/arms ride forward of the waist. ---
    body  = make_empty("body",  None,  (0, 0, 0.0))
    chest = make_empty("chest", body,  CHEST_LOC)
    head  = make_empty("head",  chest, (0, 0.10, 0.38))
    jaw   = make_empty("jaw",   head,  (0, 0.10, -0.12))
    armL  = make_empty("armL",  chest, (0.46, -0.02, 0.26))   # holds the club
    armR  = make_empty("armR",  chest, (-0.46, -0.02, 0.26))
    handL = make_empty("handL", armL,  (0, 0, -0.62))
    handR = make_empty("handR", armR,  (0, 0, -0.62))
    legL  = make_empty("legL",  body,  (0.20, 0, -0.05))
    legR  = make_empty("legR",  body,  (-0.20, 0, -0.05))
    kneeL = make_empty("kneeL", legL,  (0, 0, -0.42))
    kneeR = make_empty("kneeR", legR,  (0, 0, -0.42))

    # --- WAIST (body): low belly, lower back, pelvis stay planted (don't breathe). ---
    assemble("waist", body, [
        sphere("gut",      0.36, (0, 0.12, 0.10), skin, scale=(1.05, 1.0, 0.95), subdiv=3),
        sphere("lowback",  0.30, (0, -0.10, 0.06), skin, scale=(1.1, 0.9, 0.9), subdiv=2),
        sphere("pelvis",   0.30, (0, 0.02, -0.12), skin, scale=(1.2, 1.0, 0.8), subdiv=2),
    ])
    assemble("loincloth", body, [
        box("belt",  (0.74, 0.54, 0.12), (0, 0.04, -0.02), loin, bev=0.03),
        box("apron", (0.42, 0.10, 0.42), (0, 0.24, -0.22), loin, bev=0.03),
    ])

    # --- CHEST (breathes): barrel ribcage, slab pecs, traps; dark hunched hump. ---
    assemble("torso", chest, [
        sphere("barrel", 0.40, (0, 0.06, 0.06), skin, scale=(1.25, 1.0, 0.95), subdiv=3),
        sphere("pecL",   0.18, (0.17, 0.20, 0.10), skin, subdiv=2),
        sphere("pecR",   0.18, (-0.17, 0.20, 0.10), skin, subdiv=2),
        sphere("traps",  0.18, (0, -0.04, 0.30), skin, scale=(1.7, 1.0, 0.7), subdiv=2),
    ])
    assemble("hump", chest, [
        sphere("hump", 0.28, (0, -0.26, 0.20), dark, scale=(1.2, 1.0, 1.1), subdiv=3),
    ])

    # --- HEAD (rides the chest): cranium, heavy muzzle, cheeks, ears; brow + nostrils dark;
    #     glowing eyes. Lower jaw + tusks live on the JAW bone so they drop when it breathes. ---
    assemble("skull", head, [
        sphere("cranium", 0.22, (0, -0.02, 0.06), skin, scale=(1.0, 1.05, 1.0), subdiv=3),
        sphere("muzzle",  0.17, (0, 0.16, -0.06), skin, scale=(1.05, 1.1, 0.85), subdiv=2),
        sphere("cheekL",  0.10, (0.16, 0.08, -0.02), skin, subdiv=2),
        sphere("cheekR",  0.10, (-0.16, 0.08, -0.02), skin, subdiv=2),
        sphere("earL",    0.07, (0.23, -0.06, 0.06), skin, scale=(0.6, 1.0, 1.3), subdiv=2),
        sphere("earR",    0.07, (-0.23, -0.06, 0.06), skin, scale=(0.6, 1.0, 1.3), subdiv=2),
        sphere("noseblob", 0.08, (0, 0.26, 0.0), skin, subdiv=2),
    ])
    assemble("brow", head, [
        box("browridge", (0.42, 0.14, 0.10), (0, 0.20, 0.14), dark, bev=0.02),
        sphere("nostrilL", 0.03, (0.05, 0.30, -0.02), dark, scale=(1.0, 0.8, 1.2), subdiv=1),
        sphere("nostrilR", 0.03, (-0.05, 0.30, -0.02), dark, scale=(1.0, 0.8, 1.2), subdiv=1),
    ])
    assemble("eyes", head, [
        sphere("eyeL", 0.05, (0.11, 0.22, 0.08), eye, subdiv=2),
        sphere("eyeR", 0.05, (-0.11, 0.22, 0.08), eye, subdiv=2),
    ])

    # --- JAW: protruding underbite + two upthrust tusks and lower teeth (nail). ---
    assemble("jawmass", jaw, [
        sphere("jawbone", 0.16, (0, 0.06, -0.02), skin, scale=(1.1, 1.1, 0.7), subdiv=2),
        sphere("chin",    0.08, (0, 0.16, -0.06), skin, subdiv=2),
    ])
    teeth = [
        bone_cyl("tuskL", 0.05, 0.0, 0.24, (0.12, 0.14, 0.08), nail, rot=(0, -12, 0), segs=8),
        bone_cyl("tuskR", 0.05, 0.0, 0.24, (-0.12, 0.14, 0.08), nail, rot=(0, 12, 0), segs=8),
    ]
    for k in range(3):
        x = -0.07 + k * 0.07
        teeth.append(bone_cyl("ltooth%d" % k, 0.025, 0.0, 0.10, (x, 0.18, 0.02), nail, segs=6))
    assemble("teeth", jaw, teeth)

    # --- ARMS (long, thick): deltoid + tapered biceps + forearm bulge per arm bone. ---
    def upperarm(side, bonenode):
        assemble("arm%s" % side, bonenode, [
            sphere("delt%s" % side, 0.21, (0, 0, 0.02), skin, scale=(1.1, 1.0, 1.0), subdiv=3),
            bone_cyl("biceps%s" % side, 0.13, 0.17, 0.56, (0, 0.02, -0.30), skin, segs=12),
            sphere("forearm%s" % side, 0.15, (0, 0.02, -0.52), skin, subdiv=2),
        ])
    upperarm("L", armL); upperarm("R", armR)

    # --- FISTS: big knuckled fist per hand bone. ---
    def fist(side, bonenode):
        parts = [sphere("fist%s" % side, 0.18, (0, 0.02, -0.04), skin, scale=(1.0, 1.1, 1.0), subdiv=3)]
        for k in range(4):
            x = -0.09 + k * 0.06
            parts.append(sphere("knuck%s%d" % (side, k), 0.05, (x, 0.17, 0.0), skin, subdiv=2))
        assemble("hand%s" % side, bonenode, parts)
    fist("L", handL); fist("R", handR)

    # --- STONE-PILLAR CLUB in the LEFT hand (so the masked-armL punch swings it). Shaft (stone)
    #     joins to one primitive; head + four spikes (dstone) join to another. ---
    assemble("clubshaft", handL, [
        bone_cyl("shaft", 0.16, 0.12, 1.30, (0, 0, -0.85), stone, segs=12),
    ])
    spikes = [sphere("clubhead", 0.30, (0, 0, -1.55), dstone, subdiv=3)]
    for px, py, rot in [(0.30, 0, (0, 90, 0)), (-0.30, 0, (0, -90, 0)),
                        (0, 0.30, (-90, 0, 0)), (0, -0.30, (90, 0, 0))]:
        spikes.append(bone_cyl("spike", 0.11, 0.0, 0.28, (px, py, -1.55), dstone, rot=rot, segs=8))
    assemble("clubhead", handL, spikes)

    # --- LEGS: thigh on the leg bone, shin+foot(+claws) on the knee bone (so the knee bends). ---
    def thigh(side, bonenode):
        assemble("thigh%s" % side, bonenode, [
            sphere("hip%s" % side, 0.17, (0, 0, 0.04), skin, subdiv=3),
            bone_cyl("femur%s" % side, 0.14, 0.19, 0.42, (0, 0, -0.21), skin, segs=12),
            sphere("kneecap%s" % side, 0.15, (0, 0.02, -0.42), skin, subdiv=2),
        ])
    def shin(side, bonenode):
        assemble("shin%s" % side, bonenode, [
            bone_cyl("tibia%s" % side, 0.13, 0.15, 0.40, (0, 0, -0.22), skin, segs=12),
            sphere("ankle%s" % side, 0.12, (0, 0, -0.44), skin, subdiv=2),
        ])
        assemble("foot%s" % side, bonenode, [
            box("sole%s" % side, (0.30, 0.48, 0.18), (0, 0.12, -0.50), dark, bev=0.03),
        ])
        claws = []
        for k in range(3):
            x = -0.09 + k * 0.09
            claws.append(bone_cyl("claw%s%d" % (side, k), 0.05, 0.0, 0.14, (x, 0.37, -0.55), nail,
                                  rot=(-100, 0, 0), segs=6))
        assemble("claws%s" % side, bonenode, claws)
    thigh("L", legL); shin("L", kneeL)
    thigh("R", legR); shin("R", kneeR)

    # ================= ANIMATIONS =================
    # walk: slow, heavy lumber. Legs swing wide, knees bend on the back-swing, chest rolls
    # side-to-side with the weight, the club arm swings a little, the free arm counter-swings.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 16), (16, -16), (31, 16)]:
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armR, walk, frame, (-a * 0.6, 0, 0))
        key_rot(armL, walk, frame, (a * 0.25, 0, 0))     # heavy club arm barely swings
    for frame, kl, kr in [(1, 8, 40), (16, 40, 8), (31, 8, 40)]:
        key_rot(kneeL, walk, frame, (kl, 0, 0))
        key_rot(kneeR, walk, frame, (kr, 0, 0))
    for frame, roll in [(1, 5), (16, -5), (31, 5)]:        # lumbering weight shift
        key_rot(chest, walk, frame, (0, roll, 0))

    # idle: ALIVE. Heavy breathing (chest heaves up + leans), slow weight-shift sway (chest
    # roll), head loll, jaw drops on the breath, shoulders heave with the chest. Feet stay
    # planted because only the chest (not the waist/legs) moves.
    idle = bpy.data.actions.new("idle")
    bx, by, bz = CHEST_LOC
    for frame, lean, roll, rise in [(1, 0, 0, 0.0), (21, -2.0, 2.5, 0.022),
                                    (41, -4.0, 0.0, 0.045), (61, -2.0, -2.5, 0.022),
                                    (81, 0, 0, 0.0)]:
        key_loc(chest, idle, frame, (bx, by, bz + rise))
        key_rot(chest, idle, frame, (lean, roll, 0))
    for frame, nod, hroll in [(1, 0, 0), (21, 3, -3), (41, 6, 0), (61, 3, 3), (81, 0, 0)]:
        key_rot(head, idle, frame, (nod, 0, hroll))
    for frame, drop in [(1, 0), (41, -9), (81, 0)]:        # jaw hangs open on the inhale
        key_rot(jaw, idle, frame, (drop, 0, 0))
    for frame, ax, az in [(1, 3, 4), (41, 7, 7), (81, 3, 4)]:  # shoulders heave outward
        key_rot(armL, idle, frame, (ax, 0, az))
        key_rot(armR, idle, frame, (ax, 0, -az))

    # punch: a BIG overhead club smash with WEIGHT. The engine plays this clip only DURING the
    # wind-up (sampled NORMALIZED against TROLL_WINDUP), so the SLAM CONTACT must be the FINAL
    # keyframe. armL raises the club way back then whips it down; the torso winds back+twists
    # then lurches forward+untwists; the wrist follows through; the legs counterbalance.
    punch = bpy.data.actions.new("punch")
    for frame, a in [(1, 0), (11, -120), (30, 85)]:        # armL: raise back -> slam CONTACT at end
        key_rot(armL, punch, frame, (a, 0, 0))
    for frame, a in [(1, 0), (11, -30), (30, 35)]:          # wrist whips the club through
        key_rot(handL, punch, frame, (a, 0, 0))
    for frame, lean, twist in [(1, 0, 0), (11, -22, 14), (30, 30, -16)]:
        key_rot(body, punch, frame, (lean, 0, twist))
    for frame, l in [(1, 0), (11, 18), (30, -22)]:          # legs counterbalance the swing
        key_rot(legL, punch, frame, (l, 0, 0))
        key_rot(legR, punch, frame, (l, 0, 0))

    # Park every action off, then stash each on its own NLA track so ALL clips export.
    for obj in (body, chest, head, jaw, armL, armR, handL, legL, legR, kneeL, kneeR):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk); stash(armL, walk); stash(armR, walk)
    stash(kneeL, walk); stash(kneeR, walk); stash(chest, walk)
    stash(chest, idle); stash(head, idle); stash(jaw, idle); stash(armL, idle); stash(armR, idle)
    stash(armL, punch); stash(handL, punch); stash(body, punch); stash(legL, punch); stash(legR, punch)

    # The punch/idle clips END on non-neutral poses. Reset every animated bone to neutral (and
    # the chest back to its rig offset) and park on frame 1 so the exporter bakes an UPRIGHT
    # rest pose, not a slammed/mid-breath one. Engine clips are full TRS overrides anyway.
    for obj in (body, chest, head, jaw, armL, armR, handL, legL, legR, kneeL, kneeR):
        obj.rotation_euler = (0, 0, 0)
    chest.location = Vector(CHEST_LOC)
    bpy.context.scene.frame_set(1)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "troll.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_troll] wrote", out_path)


if __name__ == "__main__":
    main()
