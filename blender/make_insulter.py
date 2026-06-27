"""
Build the INSULTER enemy model -> assets/models/insulter.glb.

A loud heckler who shouts insults and gestures rudely at the player. Bill-Burr-ish:
BALD, a SHORT RED/GINGER beard (close-cropped, hugs the jaw + a connected mustache), and a
LEAN / average build -- NOT fat. An everyman in a t-shirt and jeans. All of his attitude is
carried by POSE and GESTURE (a cocky swagger, a jabbing point, a "get outta here" sweep) --
NOT by the face.

CRITICAL FACE RULE: a procedural human face reads creepy, so we draw NO eyes, NO nose, NO
mouth, NO ears, NO angry brows, and NO open dark mouth cavity. The ONLY facial feature is the
short red beard on the lower face; the bald head above it is just smooth skin.

Engine pipeline (see docs/blender-model-scripting.md):
  * No GPU skinning. Each glTF *primitive* is a rigid "part" pinned to the NODE that holds
    the mesh, drawn at placement * partNodeWorld -> the rig is a hierarchy of EMPTIES and
    meshes are parented to those bones.
  * A glTF primitive == one (mesh, material) pair, so ALL same-material geometry joined onto
    one bone collapses into ONE primitive == ONE draw call. We group by (bone, material).
  * Clips routed by name: idle / walk / punch (others ignored). `idle` is the looping base
    layer the engine plays when standing still; walk replaces it when moving.

RIG: body(waist) -> chest -> head,  chest -> armL/armR -> handL/handR
     body -> legL -> kneeL,  body -> legR -> kneeR
The CHEST bone lets the upper body breathe & weight-shift without lifting the planted feet;
the KNEE bones bend through the stride.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_insulter.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

SKIN  = (0.86, 0.67, 0.54, 1.0)   # bald head / forearms / hands
BEARD = (0.66, 0.31, 0.13, 1.0)   # short red/ginger beard + mustache
SHIRT = (0.46, 0.50, 0.55, 1.0)   # heather-grey t-shirt
JEANS = (0.23, 0.29, 0.44, 1.0)   # blue jeans
SHOE  = (0.10, 0.09, 0.08, 1.0)   # dark shoes

CHEST_Z = 0.42   # chest-bone height above the waist


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

def box(name, size, loc, mat_, rot=None, bev=0.012, scale=(1, 1, 1)):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, scale, False)


def assemble(name, bone, objs):
    """Bake transforms and JOIN same-material geometry into ONE object == one primitive."""
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
    """Keyframe a node's LOCAL translation (must include the bone's base offset)."""
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.location = Vector(loc)
    obj.keyframe_insert(data_path="location", frame=frame)


def main():
    reset_scene()
    skin  = mat("skin", SKIN)
    beard = mat("beard", BEARD)
    shirt = mat("shirt", SHIRT)
    jeans = mat("jeans", JEANS)
    shoe  = mat("shoe", SHOE)

    # --- Rig (empties). FRONT faces +Y. Lean shoulders (narrower than the old stocky one). ---
    body  = make_empty("body",  None,  (0, 0, 0.0))
    chest = make_empty("chest", body,  (0, 0, CHEST_Z))
    head  = make_empty("head",  chest, (0, 0, 0.50))         # abs 0.92
    armL  = make_empty("armL",  chest, (0.25, 0, 0.38))      # abs 0.80
    armR  = make_empty("armR",  chest, (-0.25, 0, 0.38))
    handL = make_empty("handL", armL,  (0, 0, -0.52))
    handR = make_empty("handR", armR,  (0, 0, -0.52))
    midR  = make_empty("midR",  handR, (0, 0.05, -0.16))   # middle-finger knuckle: curled at rest, EXTENDS on the attack
    legL  = make_empty("legL",  body,  (0.13, 0, 0.0))
    legR  = make_empty("legR",  body,  (-0.13, 0, 0.0))
    kneeL = make_empty("kneeL", legL,  (0, 0, -0.52))
    kneeR = make_empty("kneeR", legR,  (0, 0, -0.52))

    cz = lambda z: z - CHEST_Z   # absolute height -> chest-local

    # --- CHEST: a LEAN t-shirt torso (no gut). A rounded upper chest tapering into a normal
    # waist, with regular (not bulbous) shoulders. All shirt -> one primitive. Rides the chest
    # bone so it breathes / weight-shifts in idle. ---
    assemble("torso", chest, [
        # A FULL, fleshed-out t-shirt torso (no spine/concave look, no pec "boobs"): a deep
        # rounded chest box + a filling belly/back sphere give it real volume front and back.
        box("chestbox", (0.46, 0.40, 0.34), (0, 0.0, cz(0.62)), shirt, bev=0.14),
        sphere("torsofill", 0.215, (0, 0.0, cz(0.60)), shirt, scale=(1.05, 1.0, 1.0), subdiv=3),
        sphere("belly", 0.19, (0, 0.05, cz(0.40)), shirt, scale=(1.05, 0.95, 0.95), subdiv=3),
        sphere("shoulL", 0.115, (0.23, 0, cz(0.78)), shirt, subdiv=2),  # regular shoulder
        sphere("shoulR", 0.115, (-0.23, 0, cz(0.78)), shirt, subdiv=2),
        bone_cyl("collar", 0.105, 0.085, 0.13, (0, 0, cz(0.86)), shirt, segs=12),  # t-shirt collar
    ])

    # --- WAIST/PELVIS on the body bone (jeans) -- stays planted while the chest breathes. ---
    assemble("waist", body, [
        box("belt", (0.40, 0.27, 0.11), (0, 0, 0.16), jeans, bev=0.04),
        sphere("hipL", 0.12, (0.13, 0, 0.09), jeans, subdiv=2),
        sphere("hipR", 0.12, (-0.13, 0, 0.09), jeans, subdiv=2),
        sphere("seat", 0.15, (0, -0.09, 0.11), jeans, scale=(1.1, 0.9, 0.85), subdiv=2),
    ])

    # --- HEAD (skin): a smooth BALD cranium + lower-face mass + neck. NO eyes, nose, ears,
    # brows -- just smooth skin above the beard. ---
    assemble("skull", head, [
        sphere("cranium", 0.195, (0, 0.0, 0.10), skin, scale=(1.0, 1.06, 1.14), subdiv=3),   # ONE sphere = the whole head
        bone_cyl("neck",  0.085, 0.10, 0.13, (0, -0.01, -0.16), skin, segs=12),
    ])

    # --- BEARD (red): the ONLY facial feature. A short, close-cropped beard hugging the jaw
    # from sideburn -> jaw -> chin -> jaw -> sideburn, plus a connected mustache. Thin/flattened
    # against the skin so it reads neat & cropped, not bushy. ---
    assemble("beard", head, [
        # chin / under-lip tuft
        sphere("chin",   0.085, (0, 0.165, -0.07), beard, scale=(1.35, 1.0, 0.95), subdiv=2),
        # jaw line (both sides), hugging the lower face
        sphere("jawL",   0.075, (0.115, 0.135, -0.05), beard, scale=(0.85, 1.0, 1.1), subdiv=2),
        sphere("jawR",   0.075, (-0.115, 0.135, -0.05), beard, scale=(0.85, 1.0, 1.1), subdiv=2),
        # cheek beard climbing toward the sideburns (short)
        sphere("cheekL", 0.062, (0.155, 0.075, 0.05), beard, scale=(0.7, 1.0, 1.1), subdiv=2),
        sphere("cheekR", 0.062, (-0.155, 0.075, 0.05), beard, scale=(0.7, 1.0, 1.1), subdiv=2),
        # sideburns tying the beard up to where the hairline would be
        sphere("sideL",  0.05, (0.17, 0.02, 0.13), beard, scale=(0.6, 0.9, 1.1), subdiv=2),
        sphere("sideR",  0.05, (-0.17, 0.02, 0.13), beard, scale=(0.6, 0.9, 1.1), subdiv=2),
        # under-jaw band joining the two sides beneath the chin
        box("jawband",   (0.26, 0.16, 0.06), (0, 0.08, -0.08), beard, rot=(8, 0, 0), bev=0.04),
    ])

    # --- ARMS: shirt short-sleeve cap (shoulder) + bare skin forearm. ---
    def arm(side, bonenode):
        assemble("sleeve%s" % side, bonenode, [
            sphere("deltoid%s" % side, 0.095, (0, 0, -0.01), shirt, subdiv=2),
            bone_cyl("sleeve%s" % side, 0.095, 0.08, 0.20, (0, 0, -0.13), shirt, segs=12),
        ])
        assemble("fore%s" % side, bonenode, [
            sphere("elbow%s" % side, 0.072, (0, 0, -0.24), skin, subdiv=2),
            bone_cyl("forearm%s" % side, 0.068, 0.06, 0.30, (0, 0, -0.38), skin, segs=12),
        ])
    arm("L", armL); arm("R", armR)

    # --- HANDS: the LEFT hand POINTS an accusing index finger (the rude jab); the RIGHT is a
    # shaken fist. ---
    # LEFT hand: a plain fist (the right hand is the one that flips the bird).
    assemble("handL", handL, [
        sphere("palmL",  0.088, (0, 0.0, -0.11), skin, scale=(1.0, 1.1, 1.0), subdiv=2),
        sphere("knuckL", 0.052, (0, 0.06, -0.165), skin, scale=(1.2, 1.0, 0.6), subdiv=2),
        sphere("thumbL", 0.032, (-0.065, 0.05, -0.10), skin, subdiv=1),
    ])
    # RIGHT hand: a full fist (palm + curled-finger knuckles + thumb). The middle finger is its
    # OWN bone (midR) so it stays curled in the fist normally and EXTENDS on the attack.
    assemble("handR", handR, [
        sphere("palmR",  0.088, (0, 0.0, -0.11), skin, scale=(1.0, 1.1, 1.0), subdiv=2),
        sphere("knuckR", 0.052, (0, 0.06, -0.165), skin, scale=(1.2, 1.0, 0.6), subdiv=2),   # curled fingers
        sphere("thumbR", 0.034, (0.065, 0.05, -0.10), skin, subdiv=1),
    ])
    assemble("midfingR", midR, [
        bone_cyl("midfing", 0.027, 0.020, 0.16, (0, 0, -0.08), skin, segs=8),   # the middle finger, extends out -Z from the knuckle
    ])

    # --- LEGS: jeans thigh on the leg bone; shin + shoe on the knee bone so the knee bends. ---
    def thigh(side, bonenode):
        assemble("thigh%s" % side, bonenode, [
            sphere("hipj%s" % side, 0.095, (0, 0, 0.0), jeans, subdiv=2),
            bone_cyl("femur%s" % side, 0.105, 0.09, 0.50, (0, 0, -0.27), jeans, segs=12),
        ])
    def shin(side, bonenode):
        assemble("shin%s" % side, bonenode, [
            sphere("kneej%s" % side, 0.082, (0, 0, 0.0), jeans, subdiv=2),
            bone_cyl("tibia%s" % side, 0.082, 0.072, 0.46, (0, 0, -0.25), jeans, segs=12),
        ])
        assemble("shoe%s" % side, bonenode, [
            box("shoe%s" % side, (0.16, 0.30, 0.13), (0, 0.07, -0.49), shoe, bev=0.05),
        ])
    thigh("L", legL); shin("L", kneeL)
    thigh("R", legR); shin("R", kneeR)

    # =========================== ANIMATIONS ===========================
    # walk: a normal stride -- legs/arms swing opposite, knees flex through the back-swing.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 24), (12, -24), (24, 24)]:
        key_rot(legL, walk, frame, (a, 0, 0));        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a * 0.7, 0, 0)); key_rot(armR, walk, frame, (a * 0.7, 0, 0))
    for frame, kl, kr in [(1, 8, 44), (6, 34, 22), (12, 44, 8), (18, 22, 34), (24, 8, 44)]:
        key_rot(kneeL, walk, frame, (kl, 0, 0)); key_rot(kneeR, walk, frame, (kr, 0, 0))

    # idle: COCKY but lean & alive (not frantic) -- a weight-shift swagger, the chest breathing,
    # a slow head bob/turn, and a small dismissive flick of the left (pointing) hand.
    idle = bpy.data.actions.new("idle")
    # whole-body weight shift (root roll + side slide + slight twist) -- the swagger.
    for frame, roll, slide, twist in [(1, -3, -0.025, -2), (30, 3, 0.025, 2), (60, -3, -0.025, -2)]:
        key_rot(body, idle, frame, (0, roll, twist))
        key_loc(body, idle, frame, (slide, 0, 0))
    # chest breathes (slight rise + lean), counter to the body sway.
    for frame, rise, lean in [(1, 0.0, 1.0), (20, 0.022, -1.0), (40, 0.0, 2.0), (60, 0.0, 1.0)]:
        key_loc(chest, idle, frame, (0, 0, CHEST_Z + rise))
        key_rot(chest, idle, frame, (lean, 0, 0))
    # head bobs and turns slowly as he sizes you up.
    for frame, pitch, yaw in [(1, 3, -6), (15, -1, 8), (30, 4, 9), (45, -1, -7), (60, 3, -6)]:
        key_rot(head, idle, frame, (pitch, 0, yaw))
    # LEFT arm: a small, lazy dismissive flick of the pointing hand (waves you off).
    for frame, lift, flick in [(1, 8, 0), (24, 22, -8), (40, 14, 4), (60, 8, 0)]:
        key_rot(armL, idle, frame, (-lift, 0, flick))
    for frame, p in [(1, 0), (24, -16), (40, 6), (60, 0)]:
        key_rot(handL, idle, frame, (p, 0, 0))     # finger flick on the dismissal
    # RIGHT arm: hangs with a subtle, cocked-back sway.
    for frame, sway, tuck in [(1, 6, -8), (30, -6, -14), (60, 6, -8)]:
        key_rot(armR, idle, frame, (sway, 0, tuck))

    # punch (his taunt): he brings his RIGHT hand UP and FLIPS THE BIRD — the right arm thrusts
    # up while the middle finger extends out of the fist at the top. A small left shake + chin
    # thrust sell it.
    MID_CURL = 140   # degrees: the middle finger folded into the fist at rest
    punch = bpy.data.actions.new("punch")
    for frame, a in [(1, 5), (5, -35), (11, 112), (18, 5)]:        # right arm winds back then thrusts up
        key_rot(armR, punch, frame, (a, 0, 0))
    key_rot(handR, punch, 1, (0, 0, 0)); key_rot(handR, punch, 11, (-18, 0, 0)); key_rot(handR, punch, 18, (0, 0, 0))
    # the middle finger SNAPS straight (extends) as the hand reaches the top, then re-curls
    key_rot(midR, punch, 1, (MID_CURL, 0, 0)); key_rot(midR, punch, 9, (0, 0, 0))
    key_rot(midR, punch, 15, (0, 0, 0));        key_rot(midR, punch, 18, (MID_CURL, 0, 0))
    for frame, a in [(1, 0), (8, -16), (18, 0)]:                   # small left-arm shake
        key_rot(armL, punch, frame, (a, 0, 8))
    for frame, p in [(1, 0), (10, -8), (18, 0)]:
        key_rot(head, punch, frame, (p, 0, 0))     # chin thrusts on the bark

    # ---- clear active actions, reset to a neutral REST pose, then NLA-stash every clip ----
    animated = (body, chest, head, armL, armR, handL, handR, legL, legR, kneeL, kneeR, midR)
    for obj in animated:
        if obj.animation_data: obj.animation_data.action = None
        obj.rotation_euler = (0, 0, 0)
    midR.rotation_euler = (math.radians(MID_CURL), 0, 0)   # rest pose: middle finger CURLED into the fist
    body.location  = Vector((0, 0, 0.0))
    chest.location = Vector((0, 0, CHEST_Z))

    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)

    stash(legL, walk); stash(legR, walk); stash(armL, walk); stash(armR, walk)
    stash(kneeL, walk); stash(kneeR, walk)
    stash(body, idle); stash(chest, idle); stash(head, idle)
    stash(armL, idle); stash(armR, idle); stash(handL, idle)
    stash(armR, punch); stash(handR, punch); stash(midR, punch)
    stash(armL, punch); stash(head, punch)

    bpy.context.scene.frame_set(1)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "insulter.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_insulter] wrote", out_path)


if __name__ == "__main__":
    main()
