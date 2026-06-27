"""
Build the SCAVENGER friendly-mob model -> assets/models/scavenger.glb.

A DAPPER GENTLEMAN who strolls the lane collecting dropped gold: a charcoal-navy tailored
suit, crisp white shirt front, a tall black TOP HAT with a gold band, a gold-rimmed MONOCLE
over his right eye (with a little dangling chain), a curled brown MUSTACHE, a black bowtie,
white shirt cuffs, and a polished WOOD CANE with a gold knob held in his right hand. Refined
and poised -- smooth rounded geometry, not blocky boxes.

Same engine conventions as the skeleton (see docs/blender-model-scripting.md):
  * NO GPU skinning. Each glTF primitive is a rigid part pinned to the EMPTY (bone) holding
    the mesh, drawn at placement * partNodeWorld. So the rig is a hierarchy of empties and
    same-material geometry parented to one bone is JOINED into one primitive == one draw call.
  * Clips routed by name: idle / walk / punch. `idle` is the looping base layer played while
    standing still; walk replaces it when moving.

RIG (keeps body/head/armL/armR/handL/handR/legL/legR; ADDS chest, kneeL/R for soft motion):
  body(waist) -> chest -> head, armL/armR(-> handL/handR)
  body -> legL(-> kneeL), legR(-> kneeR)
The CHEST bone lets the upper body breathe in idle without lifting the planted feet; the
KNEE bones bend the legs through the walk. armL is on +X, armR on -X (the cane is in the
right hand).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_scavenger.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

SUIT   = (0.10, 0.11, 0.16, 1.0)   # charcoal-navy jacket / trousers
SHIRT  = (0.92, 0.92, 0.95, 1.0)   # white shirt front + cuffs
SKIN   = (0.92, 0.78, 0.68, 1.0)   # fair human complexion -- face / hands
HAT    = (0.04, 0.04, 0.05, 1.0)   # black top hat
GOLD   = (0.95, 0.78, 0.22, 1.0)   # monocle rim + cane knob + hat band + buttons
DARK   = (0.03, 0.03, 0.04, 1.0)   # eyes / monocle lens / bowtie / shoes
STACHE = (0.18, 0.10, 0.05, 1.0)   # brown mustache
WOOD   = (0.20, 0.12, 0.07, 1.0)   # cane shaft

CHEST_Z = 0.42   # chest bone height above the waist


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
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = 2.0
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

def box(name, size, loc, mat_, rot=None, bev=0.02):
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
    """Keyframe a node's LOCAL translation. Include the bone's base offset (clips set the
    node transform absolutely, so omitting it would snap the bone to the origin)."""
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.location = Vector(loc)
    obj.keyframe_insert(data_path="location", frame=frame)


def main():
    reset_scene()
    suit  = mat("suit", SUIT)
    shirt = mat("shirt", SHIRT)
    skin  = mat("skin", SKIN)
    hat   = mat("hat", HAT)
    gold  = mat("gold", GOLD, emissive=True)
    dark  = mat("eye", DARK)
    stache= mat("stache", STACHE)
    wood  = mat("wood", WOOD)

    # --- Rig (empties). FRONT faces +Y. Upright, poised. ---
    body  = make_empty("body",  None, (0, 0, 0.0))
    chest = make_empty("chest", body, (0, 0, CHEST_Z))
    head  = make_empty("head",  chest, (0, 0, 0.50))      # world ~0.92
    armL  = make_empty("armL",  chest, (0.26, 0, 0.40))   # +X (free hand / gestures)
    armR  = make_empty("armR",  chest, (-0.26, 0, 0.40))  # -X (holds the cane)
    handL = make_empty("handL", armL, (0, 0, -0.50))
    handR = make_empty("handR", armR, (0, 0, -0.50))
    legL  = make_empty("legL",  body, (0.12, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.12, 0, 0.0))
    kneeL = make_empty("kneeL", legL, (0, 0, -0.55))
    kneeR = make_empty("kneeR", legR, (0, 0, -0.55))

    cz = lambda z: z - CHEST_Z   # absolute height -> chest-local

    # --- HEAD: literally just a SPHERICAL head (skin) + neck. No face/jaw/cheek bulges -- the
    # character is the beard + monocle + hat only. ---
    assemble("head_skin", head, [
        sphere("cranium", 0.155, (0, -0.01, 0.16), skin, scale=(1.0, 1.03, 1.0), subdiv=3),
        bone_cyl("neck",  0.06, 0.05, 0.12, (0, -0.01, -0.10), skin, segs=10),
    ])
    # MONOCLE lens (dark), up at eye level on the right (-X) side of the head, facing +Y.
    assemble("head_dark", head, [
        bone_cyl("mono_lens", 0.040, 0.040, 0.012, (-0.072, 0.150, 0.232), dark, rot=(90, 0, 0), segs=16),
    ])
    # MONOCLE rim + dangling chain + hat band -- all GOLD, one primitive.
    chain = []
    for k, (dy, dz) in enumerate([(0.0, -0.05), (-0.01, -0.10), (-0.015, -0.15)]):
        chain.append(sphere("chain%d" % k, 0.010, (-0.102, 0.150 + dy, 0.232 + dz), gold, subdiv=1))
    assemble("head_gold", head, [
        bone_cyl("mono_rim", 0.052, 0.052, 0.024, (-0.072, 0.145, 0.232), gold, rot=(90, 0, 0), segs=18),
        bone_cyl("hat_band", 0.150, 0.150, 0.030, (0, 0, 0.345), gold, segs=20),
    ] + chain)
    # Full BEARD hugging the lower front of the spherical head (brown) -- the only "face".
    assemble("head_beard", head, [
        sphere("beardM",  0.115, (0, 0.095, 0.04), stache, scale=(1.05, 0.95, 1.2), subdiv=3),
        sphere("beardL",  0.065, (0.085, 0.08, 0.07), stache, scale=(0.9, 0.9, 1.1), subdiv=2),
        sphere("beardR",  0.065, (-0.085, 0.08, 0.07), stache, scale=(0.9, 0.9, 1.1), subdiv=2),
        sphere("beardC",  0.075, (0, 0.085, -0.01), stache, subdiv=2),
        bone_cyl("stachebar", 0.020, 0.020, 0.13, (0, 0.13, 0.12), stache, rot=(0, 90, 0), segs=8),
    ])
    # TOP HAT (black): brim + tall slightly-tapered crown.
    assemble("head_hat", head, [
        bone_cyl("hat_brim", 0.215, 0.215, 0.035, (0, 0.01, 0.315), hat, segs=24),
        bone_cyl("hat_crown", 0.145, 0.135, 0.34, (0, 0, 0.51), hat, segs=24),
        bone_cyl("hat_top", 0.135, 0.140, 0.02, (0, 0, 0.69), hat, segs=24),
    ])

    # --- CHEST: tailored suit jacket (tapered torso + shoulders), rides the chest bone so it
    # breathes in idle without lifting the feet. ---
    assemble("jacket", chest, [
        bone_cyl("torso", 0.155, 0.205, 0.50, (0, -0.01, cz(0.62)), suit, segs=20),
        sphere("chesttop", 0.205, (0, -0.01, cz(0.84)), suit, scale=(1.05, 0.95, 0.7), subdiv=2),
        sphere("paunch",  0.135, (0, 0.10, cz(0.52)), suit, scale=(1.1, 0.9, 1.0), subdiv=2),
        sphere("shoulderL", 0.072, (0.19, 0, cz(0.80)), suit, subdiv=2),
        sphere("shoulderR", 0.072, (-0.19, 0, cz(0.80)), suit, subdiv=2),
        # lapels: two slim angled plates flanking the shirt opening.
        box("lapelL", (0.05, 0.04, 0.26), (0.07, 0.135, cz(0.70)), suit, rot=(0, -10, 0), bev=0.01),
        box("lapelR", (0.05, 0.04, 0.26), (-0.07, 0.135, cz(0.70)), suit, rot=(0, 10, 0), bev=0.01),
    ])
    # white SHIRT front strip + a stand collar (one shirt primitive).
    assemble("shirtfront", chest, [
        box("shirt", (0.11, 0.05, 0.40), (0, 0.135, cz(0.62)), shirt, bev=0.01),
        bone_cyl("collar", 0.075, 0.075, 0.05, (0, 0.05, cz(0.86)), shirt, segs=14),
    ])
    # black BOWTIE at the collar (dark primitive).
    assemble("bowtie", chest, [
        sphere("knot", 0.022, (0, 0.165, cz(0.82)), dark, subdiv=1),
        bone_cyl("bowL", 0.045, 0.012, 0.06, (0.045, 0.165, cz(0.82)), dark, rot=(0, -90, 0), segs=8),
        bone_cyl("bowR", 0.045, 0.012, 0.06, (-0.045, 0.165, cz(0.82)), dark, rot=(0, 90, 0), segs=8),
    ])
    # gold BUTTONS down the shirt front (gold primitive).
    assemble("buttons", chest, [
        sphere("btn%d" % i, 0.018, (0, 0.175, cz(0.66 - i * 0.12)), gold, subdiv=1)
        for i in range(3)
    ])

    # --- WAIST/PELVIS on the body bone (doesn't breathe): trousers seat + short jacket hem. ---
    assemble("pelvis", body, [
        bone_cyl("hips", 0.165, 0.150, 0.22, (0, 0, 0.11), suit, segs=18),
        sphere("seat", 0.155, (0, -0.04, 0.10), suit, scale=(1.1, 1.0, 0.85), subdiv=2),
        # jacket hem skirting the hips, a touch lower than the trousers top.
        bone_cyl("hem", 0.20, 0.175, 0.10, (0, 0, 0.20), suit, segs=20),
    ])

    # --- ARMS: tapered suit sleeve + a shoulder cap (suit primitive). ---
    def sleeve(side, bonenode):
        assemble("sleeve%s" % side, bonenode, [
            sphere("shldcap%s" % side, 0.082, (0, 0, 0.0), suit, subdiv=2),
            bone_cyl("upper%s" % side, 0.07, 0.082, 0.30, (0, 0, -0.17), suit, segs=12),
            bone_cyl("lower%s" % side, 0.062, 0.07, 0.24, (0, 0, -0.40), suit, segs=12),
        ])
    sleeve("L", armL); sleeve("R", armR)

    # --- HANDS: white shirt CUFF (shirt) + skin palm with finger nubs (skin). Right hand also
    # grips the CANE (wood shaft + gold knob). ---
    def hand(side, bonenode, with_cane):
        assemble("cuff%s" % side, bonenode, [
            bone_cyl("cuff%s" % side, 0.066, 0.066, 0.05, (0, 0, -0.02), shirt, segs=14),
        ])
        skinparts = [
            sphere("palm%s" % side, 0.062, (0, 0.01, -0.11), skin, scale=(1.0, 0.9, 1.1), subdiv=2),
        ]
        for k, x in enumerate((-0.035, -0.012, 0.012, 0.035)):
            skinparts.append(bone_cyl("fing%s%d" % (side, k), 0.016, 0.012, 0.085,
                                      (x, 0.03, -0.17), skin, rot=(20, 0, 0), segs=6))
        ts = -1.0 if side == "L" else 1.0   # thumb points INWARD (toward the body centerline) on each hand
        skinparts.append(bone_cyl("thumb%s" % side, 0.018, 0.013, 0.07,
                                  (0.05 * ts, 0.0, -0.12), skin, rot=(0, 55 * ts, 0), segs=6))
        assemble("hand%s" % side, bonenode, skinparts)
        if with_cane:
            assemble("cane%s" % side, bonenode, [
                bone_cyl("cane_shaft", 0.028, 0.024, 1.45, (0, 0.16, -0.62), wood, segs=12),
            ])
            assemble("caneknob%s" % side, bonenode, [
                sphere("cane_knob", 0.055, (0, 0.16, 0.14), gold, subdiv=2),
            ])
    hand("L", handL, False)
    hand("R", handR, True)

    # --- LEGS: tapered trouser thigh (leg bone) + shin (knee bone); black shoe on the knee. ---
    def thigh(side, bonenode):
        assemble("thigh%s" % side, bonenode, [
            sphere("hip%s" % side, 0.085, (0, 0, 0.02), suit, subdiv=2),
            bone_cyl("femur%s" % side, 0.072, 0.09, 0.52, (0, 0, -0.27), suit, segs=12),
            sphere("knee%s" % side, 0.066, (0, 0, -0.55), suit, subdiv=2),
        ])
    def shin(side, bonenode):
        assemble("shin%s" % side, bonenode, [
            bone_cyl("tibia%s" % side, 0.058, 0.072, 0.46, (0, 0, -0.25), suit, segs=12),
        ])
        assemble("shoe%s" % side, bonenode, [   # polished black oxford
            box("shoe%s" % side, (0.13, 0.26, 0.10), (0, 0.06, -0.49), dark, bev=0.03),
            sphere("toe%s" % side, 0.07, (0, 0.17, -0.49), dark, scale=(1.0, 1.3, 0.7), subdiv=2),
        ])
    thigh("L", legL); shin("L", kneeL)
    thigh("R", legR); shin("R", kneeR)

    # ----------------------------- ANIMATIONS -----------------------------
    # walk: an unhurried gentleman's stride. Legs swing opposite with bending knees; the LEFT
    # (free) arm swings while the RIGHT keeps the cane mostly planted; the body bobs gently.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 20), (16, -20), (32, 20)]:
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a * 0.7, 0, 0))
        key_rot(armR, walk, frame, (a * 0.25, 0, 0))   # cane just taps along
    for frame, kl, kr in [(1, 8, 40), (8, 34, 20), (16, 40, 8), (24, 20, 34), (32, 8, 40)]:
        key_rot(kneeL, walk, frame, (kl, 0, 0))
        key_rot(kneeR, walk, frame, (kr, 0, 0))
    for frame, z in [(1, 0.0), (8, -0.02), (16, 0.0), (24, -0.02), (32, 0.0)]:
        key_loc(body, walk, frame, (0, 0, z))

    # punch: a brisk CANE JAB with the right arm (a little self-defense flourish).
    punch = bpy.data.actions.new("punch")
    for frame, ax in [(1, 0), (5, -30), (10, 80), (16, 0)]:
        key_rot(armR, punch, frame, (ax, 0, 0))
    for frame, a in [(1, 0), (10, 25), (16, 0)]:
        key_rot(handR, punch, frame, (a, 0, 0))

    # idle: a refined, genteel idle. The chest breathes (rise + lean), the head gives a poised
    # tilt and a small tip-of-the-hat nod, the body shifts its weight, and the LEFT hand makes
    # a slow polite gesture while the cane arm stays composed. Alive but classy. 96-frame loop.
    idle = bpy.data.actions.new("idle")
    cb = (0, 0, CHEST_Z)
    for frame, rise, lean in [(1, 0.0, 0.0), (32, 0.016, 1.6), (64, 0.0, 0.0), (96, 0.0, 0.0)]:
        key_loc(chest, idle, frame, (cb[0], cb[1], cb[2] + rise))
        key_rot(chest, idle, frame, (lean, 0, 0))
    # head: a dignified tilt one way, a poised nod (tip of the hat), then settle.
    for frame, ax, az in [(1, 0, 0), (28, -3, 4), (52, 6, 2), (72, 1, -3), (96, 0, 0)]:
        key_rot(head, idle, frame, (ax, 0, az))
    # body: a slow weight-shift from one foot to the other.
    for frame, az in [(1, -1.5), (48, 1.5), (96, -1.5)]:
        key_rot(body, idle, frame, (0, 0, az))
    # left (free) arm: a small, slow, polite gesture.
    for frame, ax, az in [(1, 2, 4), (40, 10, 9), (68, 4, 5), (96, 2, 4)]:
        key_rot(armL, idle, frame, (ax, 0, az))
    # right (cane) arm: barely-there sway, kept composed.
    for frame, ax in [(1, 1), (48, 3), (96, 1)]:
        key_rot(armR, idle, frame, (ax, 0, -3))

    # Park each action on its own NLA track per object so ALL clips export (not just active).
    keyed = (body, chest, head, armL, armR, handR, legL, legR, kneeL, kneeR)
    for obj in keyed:
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk); stash(armL, walk); stash(armR, walk)
    stash(kneeL, walk); stash(kneeR, walk); stash(body, walk)
    stash(armR, punch); stash(handR, punch)
    stash(chest, idle); stash(head, idle); stash(body, idle)
    stash(armL, idle); stash(armR, idle)

    # Reset every bone to its REST transform so the exported standing pose is neutral/upright.
    for obj in (body, chest, head, armL, armR, handL, handR, legL, legR, kneeL, kneeR):
        obj.rotation_euler = (0, 0, 0)
    body.location = Vector((0, 0, 0))
    chest.location = Vector(cb)
    bpy.context.scene.frame_set(1)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "scavenger.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_scavenger] wrote", out_path)


if __name__ == "__main__":
    main()
