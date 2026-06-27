"""
Build the two PLAYER CLASS models with gear baked in, and export:
  assets/models/knight_class.glb  - steel knight, helm + sword + kite shield baked in
  assets/models/wizard_class.glb   - blue-robe wizard, pointy hat + staff baked in

These REPLACE the generic player.glb for rendering a player of that class. This is the
HERO model: seen up close in third person and as remote players, so it's the highest
visual priority. Rebuilt from blocky boxes to rounded, smooth-shaded armor (icospheres +
tapered cylinders + beveled plates) for a modern "Valheim caliber" silhouette.

KEY ENGINE FACTS (see docs/blender-model-scripting.md, mirrors make_skeleton.py):
  * No GPU skinning. Each glTF *primitive* is a rigid "part" pinned to the NODE that holds
    the mesh, drawn at placement * partNodeWorld. So the rig is a hierarchy of EMPTIES and
    meshes are parented to those bones.
  * A glTF primitive == one (mesh, material) pair. So ALL same-material geometry parented to
    one bone collapses (via assemble()) into ONE primitive == ONE draw call, no matter how
    many vertices. That buys rivets/trim/knuckles/boots for free -> detail without draw calls.
  * Clips routed by name: idle / walk / punch / block (+ the knight roll/rollL/rollR full-body
    somersaults). `idle` is the looping base layer the engine plays when standing still.

RIG (existing bone names PRESERVED so gear sockets keep working; chest + knees ADDED):
  body(waist) -> chest -> head, armL/armR(-> handL/handR)
  body -> legL(-> kneeL), legR(-> kneeR)
armL holds the WEAPON (engine masks the ATTACK clip to it); armR holds the SHIELD (BLOCK
clip). The CHEST bone lets the upper body breathe in idle without lifting planted feet; the
KNEE bones let legs bend through the walk instead of swinging as stiff pegs.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_playerclass.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

CHEST_Z = 0.30   # chest bone height above the waist


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
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = 4.0
    return m


def make_empty(name, parent, loc):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = 'ARROWS'; e.empty_display_size = 0.08
    bpy.context.collection.objects.link(e)
    if parent:
        e.parent = parent; e.matrix_parent_inverse = Matrix.Identity(4)
    e.location = Vector(loc)
    return e


# --- Rounded smooth primitives (same bmesh helpers as make_skeleton.py). loc is LOCAL to the
# bone the part is assembled onto: assemble() bakes the part transform then re-parents, so the
# bone's world position is added on top. ---
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

def box(name, size, loc, mat_, rot=None, bev=0.014, scale=(1, 1, 1)):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, scale, False)


def assemble(name, bone, objs):
    """Bake each part transform, JOIN all into one mesh (one glTF primitive == one draw call),
    and rigid-parent the result to `bone` so it follows the bone's animation."""
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


def build(klass):
    reset_scene()
    wizard = (klass == "wizard")

    # --- Rig (empties). FRONT faces +Y. armL on the character's LEFT (-X) holds the weapon. ---
    body  = make_empty("body",  None,  (0, 0, 0.0))
    chest = make_empty("chest", body,  (0, 0, CHEST_Z))
    head  = make_empty("head",  chest, (0, 0, 0.32))      # world ~0.62
    armL  = make_empty("armL",  chest, (-0.26, 0, 0.20))  # LEFT  -> weapon
    armR  = make_empty("armR",  chest, (0.26, 0, 0.20))   # RIGHT -> shield
    handL = make_empty("handL", armL,  (0, 0, -0.44))
    handR = make_empty("handR", armR,  (0, 0, -0.44))
    legL  = make_empty("legL",  body,  (-0.12, 0, -0.02))
    legR  = make_empty("legR",  body,  (0.12, 0, -0.02))
    kneeL = make_empty("kneeL", legL,  (0, 0, -0.48))     # world ~-0.50
    kneeR = make_empty("kneeR", legR,  (0, 0, -0.48))

    if wizard:
        robe  = mat("p_robe",  (0.20, 0.26, 0.66, 1.0))
        drobe = mat("p_drobe", (0.13, 0.17, 0.48, 1.0))
        skin  = mat("p_skin",  (0.90, 0.74, 0.62, 1.0))
        white = mat("p_white", (0.93, 0.93, 0.96, 1.0))
        hat   = mat("p_hat",   (0.16, 0.20, 0.60, 1.0))
        star  = mat("p_star",  (0.98, 0.85, 0.25, 1.0))
        wood  = mat("p_wood",  (0.45, 0.29, 0.14, 1.0))
        orb   = mat("p_orb",   (0.40, 0.78, 1.0, 1.0), emissive=True)

        # HEAD: face + flowing beard + pointy hat (cone of shrinking cylinders, drooping fwd).
        assemble("face", head, [
            sphere("skull", 0.165, (0, 0.02, 0.0), skin, scale=(1.0, 0.98, 1.06), subdiv=3),
            sphere("noseW", 0.04,  (0, 0.16, -0.02), skin, subdiv=1),
        ])
        assemble("beard", head, [
            sphere("beardM", 0.14, (0, 0.10, -0.16), white, scale=(1.0, 0.9, 1.4), subdiv=3),
            sphere("brow",   0.13, (0, 0.06, 0.10),  white, scale=(1.0, 0.55, 0.4), subdiv=2),
            bone_cyl("stache", 0.05, 0.02, 0.16, (0, 0.15, -0.05), white, rot=(90, 0, 0), segs=8),
        ])
        assemble("hat", head, [
            bone_cyl("brim",  0.30, 0.30, 0.05, (0, 0.0,  0.16), hat, segs=24),
            bone_cyl("hatA",  0.21, 0.16, 0.18, (0, 0.0,  0.27), hat, segs=20),
            bone_cyl("hatB",  0.16, 0.10, 0.18, (0, 0.03, 0.43), hat, segs=18),
            bone_cyl("hatC",  0.10, 0.045, 0.18, (0, 0.08, 0.58), hat, segs=16),
            sphere("hatTip",  0.05, (0, 0.13, 0.68), hat, subdiv=2),
        ])
        assemble("hatstars", head, [
            sphere("star0", 0.035, (0.08, 0.20, 0.33),  star, subdiv=1),
            sphere("star1", 0.030, (-0.06, 0.21, 0.41), star, subdiv=1),
            sphere("star2", 0.028, (0.02, 0.16, 0.50),  star, subdiv=1),
        ])

        # CHEST: robe torso + (subtler, lower) shoulders + a lowered collar that clears the head.
        assemble("torso", chest, [
            box("robeUp", (0.40, 0.32, 0.54), (0, 0, 0.02), robe, bev=0.06),
            sphere("shdL", 0.135, (-0.205, 0, 0.12), robe, subdiv=3, scale=(1.0, 1.0, 0.82)),
            sphere("shdR", 0.135, (0.205, 0, 0.12),  robe, subdiv=3, scale=(1.0, 1.0, 0.82)),
        ])
        assemble("collar", chest, [
            box("collar", (0.26, 0.28, 0.09), (0, 0, 0.22), drobe, bev=0.03),   # lowered so it doesn't clip the beard/head
        ])

        # BODY (waist): A-line robe skirt that flares WIDER toward the feet (narrow at the
        # waist) + a matching hem. (Previously tapered the wrong way -> narrow crown + wide
        # hem brim = a "top hat" silhouette over the legs.)
        assemble("skirt", body, [
            bone_cyl("robeLo", 0.47, 0.29, 0.84, (0, 0, -0.56), robe, segs=20),   # r_bot(feet) wide, r_top(waist) narrow
        ])
        assemble("hemsash", body, [
            bone_cyl("hem",  0.49, 0.47, 0.10, (0, 0, -0.97), drobe, segs=20),     # continues the flare, no protruding brim
            box("sash", (0.42, 0.34, 0.12), (0, 0.0, 0.02), drobe, bev=0.03),
        ])

        # ARMS: tapering robe sleeves widening to a cuff.
        for side, ab in [("L", armL), ("R", armR)]:
            assemble("sleeve%s" % side, ab, [
                sphere("shldr%s" % side, 0.11, (0, 0, 0.04), robe, subdiv=2),
                bone_cyl("arm%s" % side, 0.10, 0.13, 0.46, (0, 0, -0.22), robe, segs=12),
            ])
        # HANDS: skin. Staff + glowing orb baked into the LEFT (weapon) hand.
        assemble("handL", handL, [sphere("hndL", 0.075, (0, 0.0, -0.03), skin, subdiv=2)])
        assemble("handR", handR, [sphere("hndR", 0.075, (0, 0.0, -0.03), skin, subdiv=2)])
        assemble("staff", handL, [
            bone_cyl("shaft", 0.038, 0.038, 1.12, (0, 0.02, -0.56), wood, segs=10),
            sphere("knob", 0.05, (0, 0.02, 0.02), wood, subdiv=2),
        ])
        assemble("orb", handL, [sphere("orb", 0.13, (0, 0.02, -1.10), orb, subdiv=3)])

        # LEGS: slim robe legs (mostly hidden under hem) + simple shoes on the knee bone.
        for side, lb, kb in [("L", legL, kneeL), ("R", legR, kneeR)]:
            assemble("leg%s" % side, lb, [
                bone_cyl("thigh%s" % side, 0.11, 0.10, 0.46, (0, 0, -0.24), robe, segs=10),
            ])
            assemble("shoe%s" % side, kb, [
                bone_cyl("shin%s" % side, 0.10, 0.085, 0.42, (0, 0, -0.22), robe, segs=10),
                sphere("toe%s" % side, 0.10, (0, 0.10, -0.46), drobe, scale=(1.0, 1.7, 0.8), subdiv=2),
            ])
    else:
        steel = mat("p_steel", (0.56, 0.59, 0.66, 1.0))
        dark  = mat("p_dark",  (0.20, 0.21, 0.26, 1.0))
        skin  = mat("p_skin",  (0.90, 0.74, 0.62, 1.0))
        plume = mat("p_plume", (0.82, 0.13, 0.13, 1.0))
        blade = mat("p_blade", (0.82, 0.84, 0.92, 1.0))
        grip  = mat("p_grip",  (0.30, 0.20, 0.12, 1.0))

        # HEAD: rounded great-helm (dome + rim + nasal + rivets), visor shadow, red crest, face.
        rivetsH = [sphere("hrv%d" % i, 0.018, (math.sin(t) * 0.205, -0.01 + math.cos(t) * 0.205, -0.02),
                          steel, subdiv=1) for i, t in enumerate([0.6, 1.4, 2.2, -0.6, -1.4, -2.2])]
        assemble("helm", head, [
            sphere("dome", 0.20, (0, -0.01, 0.07), steel, scale=(1.0, 1.06, 1.04), subdiv=3),
            bone_cyl("rim", 0.205, 0.205, 0.06, (0, -0.01, -0.05), steel, segs=20),
            box("nasal", (0.05, 0.10, 0.22), (0, 0.18, 0.0), steel, bev=0.01),
        ] + rivetsH)
        assemble("visor", head, [
            box("visor", (0.34, 0.10, 0.055), (0, 0.165, 0.085), dark, bev=0.02),
            box("browband", (0.37, 0.07, 0.05), (0, 0.15, 0.15), dark, bev=0.02),
        ])
        assemble("crest", head, [
            sphere("cr%d" % i, 0.05 - i * 0.006, (0, -0.06 + i * 0.045, 0.24 - i * 0.012),
                   plume, scale=(0.5, 1.0, 1.3), subdiv=2) for i in range(6)
        ])
        assemble("face", head, [sphere("face", 0.15, (0, 0.05, -0.03), skin, scale=(1.0, 0.95, 1.05), subdiv=2)])

        # CHEST: rounded breastplate with pectoral bulges, big rounded pauldrons, gorget, rivets.
        assemble("breast", chest, [
            box("plate", (0.44, 0.30, 0.52), (0, 0.0, 0.05), steel, bev=0.05),
            sphere("pecL", 0.13, (-0.11, 0.10, 0.12), steel, scale=(1.0, 0.7, 1.0), subdiv=2),
            sphere("pecR", 0.13, (0.11, 0.10, 0.12),  steel, scale=(1.0, 0.7, 1.0), subdiv=2),
            sphere("pauldL", 0.145, (-0.265, 0, 0.16), steel, scale=(1.05, 1.05, 0.82), subdiv=3),
            sphere("pauldR", 0.145, (0.265, 0, 0.16),  steel, scale=(1.05, 1.05, 0.82), subdiv=3),
            bone_cyl("gorget", 0.13, 0.11, 0.12, (0, 0, 0.30), steel, segs=14),
            sphere("rvc0", 0.02, (-0.14, 0.16, 0.22), steel, subdiv=1),
            sphere("rvc1", 0.02, (0.14, 0.16, 0.22),  steel, subdiv=1),
        ])
        assemble("strap", chest, [
            box("strap", (0.46, 0.31, 0.07), (0, 0, 0.19), dark, bev=0.02),
            box("midline", (0.04, 0.30, 0.46), (0, 0.02, 0.04), dark, bev=0.01),
        ])

        # BODY (waist): hip plates + pelvis guard (steel); belt + faulds skirt (dark).
        assemble("pelvis", body, [
            sphere("hipL", 0.12, (-0.16, 0, -0.04), steel, scale=(1.0, 0.9, 1.1), subdiv=2),
            sphere("hipR", 0.12, (0.16, 0, -0.04),  steel, scale=(1.0, 0.9, 1.1), subdiv=2),
            box("pelv", (0.38, 0.28, 0.20), (0, 0, -0.06), steel, bev=0.04),
            box("buckle", (0.10, 0.06, 0.08), (0, 0.17, 0.04), steel, bev=0.02),
        ])
        assemble("belt", body, [
            box("belt", (0.46, 0.32, 0.10), (0, 0, 0.04), dark, bev=0.02),
            box("fauldF", (0.40, 0.10, 0.28), (0, 0.13, -0.24), dark, rot=(8, 0, 0), bev=0.02),
            box("fauldB", (0.40, 0.10, 0.28), (0, -0.13, -0.24), dark, rot=(-8, 0, 0), bev=0.02),
            box("fauldL", (0.10, 0.30, 0.28), (0.19, 0, -0.24), dark, rot=(0, -8, 0), bev=0.02),
            box("fauldR", (0.10, 0.30, 0.28), (-0.19, 0, -0.24), dark, rot=(0, 8, 0), bev=0.02),
        ])

        # ARMS: rounded rerebrace + elbow cop + vambrace (all steel) on each arm bone.
        for side, ab in [("L", armL), ("R", armR)]:
            assemble("arm%s" % side, ab, [
                sphere("shldknob%s" % side, 0.10, (0, 0, 0.03), steel, subdiv=2),
                bone_cyl("upper%s" % side, 0.085, 0.075, 0.30, (0, 0, -0.16), steel, segs=12),
                sphere("elbow%s" % side, 0.082, (0, 0, -0.31), steel, subdiv=2),
                bone_cyl("fore%s" % side, 0.078, 0.088, 0.16, (0, 0, -0.40), steel, segs=12),
            ])

        # LEFT HAND: gauntlet + knuckles (dark), wrapped grip (brown), cross-guard + pommel
        # (steel), tapered blade pointing down (blade). 4 prims, but the whole sword reads sharp.
        assemble("gauntL", handL, [
            sphere("gntL", 0.09, (0, 0, -0.03), dark, scale=(1.0, 1.0, 0.9), subdiv=2),
        ] + [sphere("knuL%d" % k, 0.022, (-0.04 + k * 0.025, 0.06, -0.07), dark, subdiv=1) for k in range(4)])
        assemble("grip", handL, [bone_cyl("grip", 0.034, 0.034, 0.20, (0, 0, -0.13), grip, segs=10)])
        assemble("hilt", handL, [
            box("guard", (0.28, 0.07, 0.05), (0, 0, -0.25), steel, bev=0.02),
            sphere("pommel", 0.045, (0, 0, -0.02), steel, subdiv=2),
        ])
        assemble("blade", handL, [
            box("fuller", (0.085, 0.030, 0.76), (0, 0, -0.65), blade, bev=0.008),
            box("tip", (0.05, 0.024, 0.16), (0, 0, -1.08), blade, bev=0.006, scale=(0.6, 1.0, 1.0)),
        ])

        # RIGHT HAND: gauntlet + boss (dark), curved kite shield (steel). 2 prims.
        assemble("gauntR", handR, [
            sphere("gntR", 0.09, (0, 0, -0.03), dark, scale=(1.0, 1.0, 0.9), subdiv=2),
            sphere("boss", 0.055, (0, 0.22, -0.10), dark, subdiv=2),
        ])
        assemble("shield", handR, [
            sphere("kiteTop", 0.25, (0, 0.16, -0.04), steel, scale=(0.82, 0.16, 1.0), subdiv=3),
            sphere("kiteBot", 0.14, (0, 0.15, -0.34), steel, scale=(1.0, 0.16, 1.5), subdiv=3),
            bone_cyl("rimring", 0.255, 0.255, 0.04, (0, 0.135, -0.06), steel, rot=(90, 0, 0), segs=20),
        ])

        # LEGS: rounded cuisse + knee poleyn (steel) on the leg bone; greave (steel) + sabaton
        # boot (dark) on the knee bone so the shin/foot follow the knee bend.
        for side, lb, kb in [("L", legL, kneeL), ("R", legR, kneeR)]:
            assemble("thigh%s" % side, lb, [
                sphere("hipj%s" % side, 0.11, (0, 0, 0.02), steel, subdiv=2),
                bone_cyl("cuisse%s" % side, 0.115, 0.10, 0.44, (0, 0, -0.23), steel, segs=12),
                sphere("poleyn%s" % side, 0.10, (0, 0.02, -0.46), steel, subdiv=2),
            ])
            assemble("greave%s" % side, kb, [
                bone_cyl("greave%s" % side, 0.10, 0.085, 0.44, (0, 0, -0.23), steel, segs=12),
            ])
            assemble("boot%s" % side, kb, [
                box("ankle%s" % side, (0.16, 0.18, 0.14), (0, 0.02, -0.48), dark, bev=0.04),
                sphere("toe%s" % side, 0.10, (0, 0.16, -0.50), dark, scale=(1.0, 1.7, 0.85), subdiv=2),
            ])

    # === ANIMATIONS ===========================================================
    # Every clip starts NEUTRAL at frame 1 so the exporter's rest sample is upright.

    # walk: legs + arms swing opposite; knees flex on the back-swing for a real stride; the
    # chest gives a tiny counter-bob. (~31-frame loop, unchanged cadence from the old version.)
    walk = bpy.data.actions.new("walk")
    for f, a in [(1, 0), (7, 22), (19, -22), (31, 0)]:
        key_rot(legL, walk, f, (a, 0, 0)); key_rot(legR, walk, f, (-a, 0, 0))
        key_rot(armL, walk, f, (-a * 0.6, 0, 0)); key_rot(armR, walk, f, (a * 0.6, 0, 0))
    for f, kl, kr in [(1, 16, 16), (7, 8, 40), (19, 40, 8), (31, 16, 16)]:
        key_rot(kneeL, walk, f, (kl, 0, 0)); key_rot(kneeR, walk, f, (kr, 0, 0))
    for f, a in [(1, 0), (7, 2), (19, 2), (31, 0)]:
        key_rot(chest, walk, f, (a, 0, 0))

    # idle: a grounded combat idle. Chest breathes (lifts + leans) while feet stay planted,
    # a slow weight-shift bends the knees a touch, shoulders/arms sway, head drifts. Alive.
    idle = bpy.data.actions.new("idle")
    for f, rise, lean in [(1, 0.0, 0.0), (31, 0.025, 3.0), (61, 0.0, 0.0)]:
        key_loc(chest, idle, f, (0, 0, CHEST_Z + rise))
        key_rot(chest, idle, f, (lean, 0, 0))
    for f, a, y in [(1, 0, 0), (31, -3, 2), (61, 0, 0)]:
        key_rot(head, idle, f, (a, y, 0))
    for f, ax, az in [(1, 0, 0), (31, 5, 6), (61, 0, 0)]:
        key_rot(armL, idle, f, (ax, 0, az)); key_rot(armR, idle, f, (ax, 0, -az))
    for f, kl, kr in [(1, 0, 0), (31, 8, 4), (61, 0, 0)]:
        key_rot(kneeL, idle, f, (kl, 0, 0)); key_rot(kneeR, idle, f, (kr, 0, 0))

    # punch: left arm winds back then swings/casts forward; wrist follows through.
    punch = bpy.data.actions.new("punch")
    for f, a in [(1, 0), (4, -35), (9, 95), (16, 0)]:
        key_rot(armL, punch, f, (a, 0, 0))
    key_rot(handL, punch, 1, (0, 0, 0)); key_rot(handL, punch, 9, (22, 0, 0)); key_rot(handL, punch, 16, (0, 0, 0))

    # block: right arm raises the shield FORWARD across the body (+X sweeps the down-hanging
    # arm toward +Y/front; the old -95 swung it backward — the "block arm moves backward" bug).
    block = bpy.data.actions.new("block")
    for f, a in [(1, 0), (8, 88)]:
        key_rot(armR, block, f, (a, 0, 20))

    # roll = STRAIGHT forward somersault (pure pitch about body X, no lean). Head tucks
    # chin-to-chest; arms/legs curl into a tight ball, knees pull up.
    roll = bpy.data.actions.new("roll")
    key_rot(body, roll, 1, (0, 0, 0)); key_rot(body, roll, 9, (-180, 0, 0)); key_rot(body, roll, 18, (-360, 0, 0))
    for f in (1, 18): key_rot(head, roll, f, (0, 0, 0))
    for f in (5, 13): key_rot(head, roll, f, (32, 0, 0))
    for f in (1, 18):
        key_rot(armL, roll, f, (0, 0, 0)); key_rot(armR, roll, f, (0, 0, 0))
        key_rot(legL, roll, f, (0, 0, 0)); key_rot(legR, roll, f, (0, 0, 0))
        key_rot(kneeL, roll, f, (0, 0, 0)); key_rot(kneeR, roll, f, (0, 0, 0))
    for f in (5, 13):
        key_rot(armL, roll, f, (78, 0, 0)); key_rot(armR, roll, f, (78, 0, 0))
        key_rot(legL, roll, f, (82, 0, 0)); key_rot(legR, roll, f, (82, 0, 0))
        key_rot(kneeL, roll, f, (60, 0, 0)); key_rot(kneeR, roll, f, (60, 0, 0))

    # rollL / rollR = SIDE rolls (barrel roll about forward Y). +Y tips crown toward +X
    # (player's right) => rollR; negative => rollL. Same tucked ball.
    def side_roll(name, sign):
        act = bpy.data.actions.new(name)
        key_rot(body, act, 1, (0, 0, 0)); key_rot(body, act, 9, (0, sign * 180, 0)); key_rot(body, act, 18, (0, sign * 360, 0))
        for f in (1, 18): key_rot(head, act, f, (0, 0, 0))
        for f in (5, 13): key_rot(head, act, f, (26, 0, 0))
        for f in (1, 18):
            key_rot(armL, act, f, (0, 0, 0)); key_rot(armR, act, f, (0, 0, 0))
            key_rot(legL, act, f, (0, 0, 0)); key_rot(legR, act, f, (0, 0, 0))
            key_rot(kneeL, act, f, (0, 0, 0)); key_rot(kneeR, act, f, (0, 0, 0))
        for f in (5, 13):
            key_rot(armL, act, f, (70, 0, 0)); key_rot(armR, act, f, (70, 0, 0))
            key_rot(legL, act, f, (80, 0, 0)); key_rot(legR, act, f, (80, 0, 0))
            key_rot(kneeL, act, f, (58, 0, 0)); key_rot(kneeR, act, f, (58, 0, 0))
        return act
    rollL = side_roll("rollL", -1)
    rollR = side_roll("rollR", 1)

    # --- Stash every clip on the bones it animates (unique track per clip/obj) so the
    # exporter writes them all by name. ---
    for obj in (body, chest, head, armL, armR, handL, handR, legL, legR, kneeL, kneeR):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk); stash(armL, walk); stash(armR, walk)
    stash(kneeL, walk); stash(kneeR, walk); stash(chest, walk)
    stash(chest, idle); stash(head, idle); stash(armL, idle); stash(armR, idle)
    stash(kneeL, idle); stash(kneeR, idle)
    stash(armL, punch); stash(handL, punch)
    stash(armR, block)
    for act in (roll, rollL, rollR):
        stash(body, act); stash(head, act); stash(armL, act); stash(armR, act)
        stash(legL, act); stash(legR, act); stash(kneeL, act); stash(kneeR, act)

    # Authoring left each bone at its last keyframe pose. Reset every bone to its NEUTRAL rest
    # (rotation 0; chest back to its base height) so the EXPORTED rest pose stands upright.
    for obj in (body, chest, head, armL, armR, handL, handR, legL, legR, kneeL, kneeR):
        obj.rotation_euler = (0, 0, 0)
    chest.location = Vector((0, 0, CHEST_Z))
    bpy.context.scene.frame_set(1)

    out = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "%s_class.glb" % klass))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_playerclass] wrote", out)


def main():
    build("knight")
    build("wizard")


if __name__ == "__main__":
    main()
