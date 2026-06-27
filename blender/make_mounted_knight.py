"""
Procedurally build a MOUNTED KNIGHT (a "Tree Sentinel"-style golden cavalier) and export
assets/models/mounted_knight.glb.

WHAT IT IS: a heavily-armored WARHORSE in full gilded plate barding, ridden by a tall steel
knight in golden armor who carries a LONG SPEAR/LANCE in the weapon hand and a BIG HEATER
SHIELD on the other arm. It REPLACES the old crude box version in main.cpp (an armored horse
+ rider that REARS up on its hind legs and SLAMS down to attack).

KEY ENGINE FACTS (see docs/blender-model-scripting.md, mirrors make_skeleton.py):
  * No GPU skinning. Each glTF *primitive* is a rigid "part" pinned to the NODE that holds the
    mesh, drawn at placement * partNodeWorld. So the rig is a hierarchy of EMPTIES and meshes
    are RIGIDLY PARENTED to those bones (NOT skinned).
  * A glTF primitive == one (mesh, material) pair. assemble() bakes + joins same-material
    geometry per bone -> ONE primitive == ONE draw call no matter how many verts. So plate
    rivets / spikes / barding cost nothing extra as long as they share a material per bone.
  * Clips routed by name: idle / walk / punch (others ignored). idle is the looping base layer
    the engine plays when standing still; walk replaces it when moving; punch is the attack.

ORIENTATION / SCALE (must match the engine):
  * Z up. FORWARD = +Y (same as every other character model; engine applies MODEL_YAW_OFFSET).
  * ORIGIN AT THE HIND HOOVES (back feet on the ground, z=0, y~=0). The root `body` bone sits
    AT the origin, so pitching `body` about X rears/slams the WHOLE rig about the hind hooves
    naturally -- exactly the pivot the old procedural version used (pvx = ax - F0*0.85).
  * Proportions echo the old boxes: leg span ~1.5 in Y, barrel ~1.05 above ground, neck/head
    arching up to ~+Y 2.2, rider seated on the saddle leaning with the rear/slam.

RIG (empties; reported names):
  body(root @ hind hooves) -> { head, frontL, frontR, backL, backR, tail, rider }
  rider -> { armL(-> handL: SPEAR), armR(-> handR: SHIELD) }
The FRONT legs (frontL/frontR) tuck/paw the air on the rear; the HIND legs (backL/backR) ride
the body pivot so the hooves stay planted. armL is the weapon arm (spear); armR carries the
big heater shield.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_mounted_knight.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

# --- palette: gilded plate (Tree Sentinel gold) over dark steel, on a barded warhorse ---
GOLD  = (0.83, 0.67, 0.22, 1.0)   # gilded plate barding + rider armor (primary)
GOLD2 = (0.62, 0.48, 0.14, 1.0)   # deeper gold for trim / shadowed plate
STEEL = (0.50, 0.53, 0.60, 1.0)   # bare steel: spear shaft + blade, shield rim, joints
DARK  = (0.15, 0.15, 0.19, 1.0)   # leather / gaps / visor / gauntlets
HORSE = (0.32, 0.22, 0.13, 1.0)   # warhorse hide (mostly hidden under barding)
HAIR  = (0.12, 0.09, 0.06, 1.0)   # mane + tail
HOOF  = (0.10, 0.09, 0.10, 1.0)   # hooves / eyes
CLOTH = (0.15, 0.20, 0.45, 1.0)   # blue caparison drape


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
        if "Metallic" in b.inputs: b.inputs["Metallic"].default_value = 0.7
        if "Roughness" in b.inputs: b.inputs["Roughness"].default_value = 0.4
        if emissive and "Emission Color" in b.inputs:
            b.inputs["Emission Color"].default_value = rgba
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = 3.0
    return m


def make_empty(name, parent, loc):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = 'ARROWS'; e.empty_display_size = 0.10
    bpy.context.collection.objects.link(e)
    if parent:
        e.parent = parent; e.matrix_parent_inverse = Matrix.Identity(4)
    e.location = Vector(loc)
    return e


# --- rounded smooth primitives (bmesh helpers, same as make_skeleton / make_playerclass).
# `loc` is LOCAL to the bone the part is assembled onto. ---
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
    """Bake each part transform, JOIN into one mesh (one primitive/material == one draw call),
    rigid-parent the result to `bone` so it follows the bone's animation."""
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
    gold  = mat("mk_gold",  GOLD)
    gold2 = mat("mk_gold2", GOLD2)
    steel = mat("mk_steel", STEEL)
    dark  = mat("mk_dark",  DARK)
    horse = mat("mk_horse", HORSE)
    hair  = mat("mk_hair",  HAIR)
    hoof  = mat("mk_hoof",  HOOF)
    cloth = mat("mk_cloth", CLOTH)

    # === RIG (empties). Origin = hind hooves on the ground. FORWARD = +Y, Z up. ===========
    body  = make_empty("body",  None, (0.0, 0.0, 0.0))     # root, AT the hind hooves (pivot)
    head  = make_empty("head",  body, (0.0, 1.35, 1.20))   # base of the neck (withers) -> arches up
    frontL = make_empty("frontL", body, ( 0.30, 1.55, 0.92))
    frontR = make_empty("frontR", body, (-0.30, 1.55, 0.92))
    backL  = make_empty("backL",  body, ( 0.30, 0.05, 0.92))
    backR  = make_empty("backR",  body, (-0.30, 0.05, 0.92))
    tail   = make_empty("tail",   body, (0.0, -0.10, 1.20))
    rider  = make_empty("rider",  body, (0.0, 0.72, 1.48))  # seated on the saddle
    armL  = make_empty("armL",  rider, (-0.26, 0.02, 0.52)) # weapon arm (SPEAR), raised
    armR  = make_empty("armR",  rider, ( 0.28, 0.04, 0.46)) # shield arm
    handL = make_empty("handL", armL,  (0.0, 0.10, 0.60))   # spear grip (top of raised forearm)
    handR = make_empty("handR", armR,  (0.0, 0.10, -0.42))  # shield grip (down-forward)

    # === WARHORSE BODY (on `body`; body is at the origin so local == model coords) ==========
    # hide: barrel + chest + croup + belly + neck-base (mostly hidden under barding).
    assemble("horsehide", body, [
        sphere("barrel", 0.42, (0.0, 0.82, 1.04), horse, scale=(0.86, 1.95, 0.92), subdiv=3),
        sphere("chest",  0.36, (0.0, 1.42, 1.02), horse, scale=(0.92, 0.85, 1.00), subdiv=3),
        sphere("croup",  0.36, (0.0, 0.18, 1.06), horse, scale=(0.95, 0.85, 0.95), subdiv=3),
        sphere("belly",  0.34, (0.0, 0.82, 0.82), horse, scale=(0.80, 1.7, 0.70), subdiv=2),
    ])
    # gilded plate BARDING: peytral (chest), croup plate (rump), flanchards (sides), spinal crest.
    assemble("barding", body, [
        # peytral across the chest/breast
        box("peytral", (0.66, 0.20, 0.52), (0.0, 1.55, 1.00), gold, rot=(8, 0, 0), bev=0.04),
        sphere("peytralB", 0.30, (0.0, 1.62, 0.86), gold, scale=(1.1, 0.5, 0.9), subdiv=3),
        # croup (rump) plate
        sphere("croupP", 0.37, (0.0, 0.16, 1.10), gold, scale=(0.98, 0.78, 0.92), subdiv=3),
        box("croupTrim", (0.50, 0.18, 0.10), (0.0, -0.06, 1.18), gold2, bev=0.03),
        # flanchards (hip side plates)
        box("flanchL", (0.10, 0.50, 0.40), ( 0.40, 0.45, 0.98), gold, rot=(0, 6, 0), bev=0.04),
        box("flanchR", (0.10, 0.50, 0.40), (-0.40, 0.45, 0.98), gold, rot=(0, -6, 0), bev=0.04),
        # spinal crest strip along the back + a row of gilded studs
        box("spine", (0.10, 1.5, 0.10), (0.0, 0.80, 1.45), gold, bev=0.03),
    ] + [sphere("stud%d" % k, 0.035, (0.0, 0.25 + k * 0.22, 1.49), gold2, subdiv=1) for k in range(6)])
    # saddle + cantle
    assemble("saddle", body, [
        box("seat", (0.46, 0.42, 0.14), (0.0, 0.72, 1.46), dark, bev=0.05),
        box("cantle", (0.40, 0.10, 0.16), (0.0, 0.54, 1.54), gold2, bev=0.04),
        box("pommelS", (0.30, 0.10, 0.14), (0.0, 0.92, 1.54), gold2, bev=0.04),
    ])
    # blue caparison drape hanging on each side, gilded hem
    assemble("caparison", body, [
        box("capL", (0.06, 1.20, 0.46), ( 0.46, 0.78, 0.66), cloth, bev=0.02),
        box("capR", (0.06, 1.20, 0.46), (-0.46, 0.78, 0.66), cloth, bev=0.02),
    ])
    assemble("caphem", body, [
        box("hemL", (0.07, 1.20, 0.08), ( 0.465, 0.78, 0.44), gold2, bev=0.02),
        box("hemR", (0.07, 1.20, 0.08), (-0.465, 0.78, 0.44), gold2, bev=0.02),
    ])

    # === HEAD + NECK (on `head`, base at the withers; arches up-forward). Local coords. =====
    assemble("neckhead", head, [
        bone_cyl("neck0", 0.26, 0.22, 0.40, (0.0, 0.14, 0.18), horse, rot=(-58, 0, 0), segs=14),
        bone_cyl("neck1", 0.22, 0.17, 0.36, (0.0, 0.36, 0.40), horse, rot=(-42, 0, 0), segs=14),
        sphere("skull", 0.18, (0.0, 0.58, 0.46), horse, scale=(0.85, 1.05, 0.95), subdiv=3),
        bone_cyl("muzzle", 0.14, 0.11, 0.30, (0.0, 0.80, 0.36), horse, rot=(-72, 0, 0), segs=12),
        sphere("jaw", 0.11, (0.0, 0.74, 0.30), horse, scale=(0.9, 1.0, 0.7), subdiv=2),
    ])
    # gilded chamfron (face armor) + forehead spike + crinet (gilded neck segments)
    assemble("chamfron", head, [
        box("faceplate", (0.24, 0.34, 0.10), (0.0, 0.72, 0.52), gold, rot=(-66, 0, 0), bev=0.03),
        bone_cyl("spike", 0.05, 0.005, 0.34, (0.0, 0.66, 0.78), gold2, rot=(-18, 0, 0), segs=10),
        sphere("poll", 0.10, (0.0, 0.50, 0.62), gold, scale=(1.1, 0.7, 0.8), subdiv=2),
    ] + [box("crinet%d" % k, (0.30 - k * 0.03, 0.10, 0.06),
             (0.0, 0.10 + k * 0.16, 0.20 + k * 0.16), gold, rot=(-50, 0, 0), bev=0.02) for k in range(3)])
    # ears (hide) + dark eyes + mane crest
    assemble("ears", head, [
        bone_cyl("earL", 0.05, 0.005, 0.16, ( 0.09, 0.50, 0.66), horse, rot=(-10, 0, 0), segs=8),
        bone_cyl("earR", 0.05, 0.005, 0.16, (-0.09, 0.50, 0.66), horse, rot=(-10, 0, 0), segs=8),
    ])
    assemble("headdark", head, [
        sphere("eyeL", 0.045, ( 0.15, 0.62, 0.50), hoof, subdiv=1),
        sphere("eyeR", 0.045, (-0.15, 0.62, 0.50), hoof, subdiv=1),
    ] + [sphere("mane%d" % k, 0.07, (0.0, 0.12 + k * 0.10, 0.30 + k * 0.085), hair,
                scale=(0.5, 0.9, 1.1), subdiv=2) for k in range(5)])

    # === LEGS: brown leg + gilded plate + dark hoof, built LOCAL (hang from the bone to z=0) ==
    def make_leg(name, bonenode, length):
        assemble(name + "_hide", bonenode, [
            bone_cyl("up", 0.12, 0.10, length * 0.55, (0, 0, -length * 0.26), horse, segs=12),
            bone_cyl("lo", 0.075, 0.055, length * 0.55, (0, 0, -length * 0.72), horse, segs=12),
            sphere("fet", 0.07, (0, 0.01, -length * 0.96), horse, subdiv=2),
        ])
        assemble(name + "_plate", bonenode, [
            sphere("knee", 0.12, (0, 0.04, -length * 0.5), gold, scale=(1.0, 1.1, 1.0), subdiv=2),
            box("shinp", (0.16, 0.10, length * 0.42), (0, 0.07, -length * 0.74), gold, bev=0.02),
        ])
        assemble(name + "_hoof", bonenode, [
            box("hoof", (0.15, 0.20, 0.12), (0, 0.03, -length * 1.0), hoof, bev=0.03),
        ])
    make_leg("frontL", frontL, 0.92); make_leg("frontR", frontR, 0.92)
    make_leg("backL",  backL,  0.92); make_leg("backR",  backR,  0.92)

    # === TAIL (hair, hangs back/down off the rump) =========================================
    assemble("tailgeo", tail, [
        bone_cyl("dock", 0.09, 0.05, 0.50, (0.0, -0.10, -0.22), hair, rot=(28, 0, 0), segs=10),
        sphere("tuft", 0.10, (0.0, -0.20, -0.46), hair, scale=(0.8, 0.9, 1.6), subdiv=2),
    ])

    # === RIDER (on `rider`, seat pivot). Tall golden knight. Local coords. =================
    assemble("rtorso", rider, [
        box("brstplate", (0.42, 0.30, 0.46), (0.0, 0.0, 0.30), gold, bev=0.05),
        sphere("pecL", 0.13, (-0.11, 0.10, 0.34), gold, scale=(1.0, 0.7, 1.0), subdiv=2),
        sphere("pecR", 0.13, ( 0.11, 0.10, 0.34), gold, scale=(1.0, 0.7, 1.0), subdiv=2),
        sphere("pauldL", 0.16, (-0.26, 0.0, 0.50), gold, scale=(1.05, 1.05, 0.85), subdiv=3),
        sphere("pauldR", 0.16, ( 0.26, 0.0, 0.50), gold, scale=(1.05, 1.05, 0.85), subdiv=3),
        bone_cyl("gorget", 0.13, 0.11, 0.12, (0.0, 0.0, 0.58), gold, segs=14),
        # faulds skirt over the saddle
        box("fauldF", (0.40, 0.10, 0.24), (0.0, 0.16, 0.02), gold, rot=(10, 0, 0), bev=0.03),
        box("fauldL", (0.10, 0.34, 0.24), ( 0.20, 0.0, 0.02), gold, rot=(0, -8, 0), bev=0.03),
        box("fauldR", (0.10, 0.34, 0.24), (-0.20, 0.0, 0.02), gold, rot=(0, 8, 0), bev=0.03),
    ])
    assemble("rdark", rider, [
        box("belt", (0.44, 0.30, 0.09), (0.0, 0.0, 0.12), dark, bev=0.02),
        box("midline", (0.05, 0.30, 0.42), (0.0, 0.02, 0.30), dark, bev=0.01),
    ])
    # legs straddling the horse (static on the rider): gilded cuisse + dark boot, angled out+down
    assemble("rlegs", rider, [
        bone_cyl("thighL", 0.13, 0.10, 0.46, (-0.20, 0.18, -0.18), gold, rot=(64, 0, 0), segs=12),
        bone_cyl("thighR", 0.13, 0.10, 0.46, ( 0.20, 0.18, -0.18), gold, rot=(64, 0, 0), segs=12),
        bone_cyl("shinL", 0.10, 0.085, 0.42, (-0.24, 0.46, -0.40), gold, rot=(95, 0, 0), segs=10),
        bone_cyl("shinR", 0.10, 0.085, 0.42, ( 0.24, 0.46, -0.40), gold, rot=(95, 0, 0), segs=10),
    ])
    assemble("rboots", rider, [
        box("bootL", (0.14, 0.22, 0.12), (-0.27, 0.66, -0.46), dark, bev=0.03),
        box("bootR", (0.14, 0.22, 0.12), ( 0.27, 0.66, -0.46), dark, bev=0.03),
    ])
    # HELM: gilded great-helm dome + rim + nasal + a tall crest fin (Tree-Sentinel look) + visor
    assemble("rhelm", rider, [
        sphere("dome", 0.18, (0.0, 0.02, 0.80), gold, scale=(1.0, 1.05, 1.10), subdiv=3),
        bone_cyl("rim", 0.185, 0.185, 0.06, (0.0, 0.02, 0.70), gold, segs=18),
        box("nasal", (0.05, 0.10, 0.20), (0.0, 0.18, 0.80), gold, bev=0.01),
    ] + [box("crest%d" % k, (0.04, 0.05 + k * 0.02, 0.10), (0.0, -0.05 - k * 0.03, 0.98 + k * 0.05),
             gold2, bev=0.01) for k in range(4)])
    assemble("rvisor", rider, [
        box("visor", (0.30, 0.10, 0.05), (0.0, 0.155, 0.84), dark, bev=0.02),
        box("brow",  (0.33, 0.07, 0.05), (0.0, 0.14, 0.90), dark, bev=0.02),
    ])

    # === WEAPON ARM (armL) + SPEAR (handL). Arm raised; spear towers up-forward. ============
    assemble("armLgeo", armL, [
        sphere("shldL", 0.11, (0, 0, 0.02), gold, subdiv=2),
        bone_cyl("upL", 0.09, 0.08, 0.32, (0, 0.04, 0.18), gold, rot=(-12, 0, 0), segs=12),
        sphere("elbowL", 0.085, (0, 0.10, 0.36), gold, subdiv=2),
        bone_cyl("foreL", 0.082, 0.075, 0.30, (0, 0.10, 0.52), gold, rot=(-6, 0, 0), segs=12),
    ])
    assemble("handLgeo", handL, [sphere("gauntL", 0.09, (0, 0, 0.0), dark, subdiv=2)])
    # SPEAR / LANCE: long steel shaft (raised, slight forward lean) + gilded collar + leaf blade.
    assemble("spearShaft", handL, [
        bone_cyl("shaft", 0.042, 0.042, 1.70, (0.0, 0.06, 0.58), steel, rot=(-7, 0, 0), segs=10),
        bone_cyl("buttcap", 0.05, 0.05, 0.10, (0.0, 0.0, -0.32), gold2, segs=10),
        bone_cyl("collar", 0.055, 0.055, 0.08, (0.0, 0.10, 1.20), gold2, segs=10),
    ])
    assemble("spearWrap", handL, [bone_cyl("grip", 0.05, 0.05, 0.20, (0.0, 0.04, 0.10), dark, segs=10)])
    assemble("spearHead", handL, [
        box("blade", (0.10, 0.05, 0.44), (0.0, 0.18, 1.52), steel, bev=0.01, scale=(1.0, 1.0, 1.0)),
        bone_cyl("tip", 0.05, 0.004, 0.20, (0.0, 0.22, 1.80), steel, rot=(-7, 0, 0), segs=8),
    ])

    # === SHIELD ARM (armR) + BIG HEATER SHIELD (handR) =====================================
    assemble("armRgeo", armR, [
        sphere("shldR", 0.11, (0, 0, 0.02), gold, subdiv=2),
        bone_cyl("upR", 0.09, 0.08, 0.30, (0, 0.05, -0.16), gold, rot=(40, 0, 0), segs=12),
        sphere("elbowR", 0.085, (0, 0.16, -0.30), gold, subdiv=2),
        bone_cyl("foreR", 0.082, 0.078, 0.26, (0, 0.24, -0.40), gold, rot=(70, 0, 0), segs=12),
    ])
    assemble("handRgeo", handR, [sphere("gauntR", 0.09, (0, 0.04, -0.02), dark, subdiv=2)])
    # heater shield: large gilded face (flat top, tapering to a point) facing +Y, steel rim + boss.
    assemble("shieldFace", handR, [
        box("shTop", (0.62, 0.10, 0.46), (0.0, 0.16, 0.30), gold, bev=0.04),
        box("shMid", (0.50, 0.10, 0.30), (0.0, 0.16, 0.02), gold, bev=0.04, scale=(1.0, 1.0, 1.0)),
        bone_cyl("shTip", 0.26, 0.03, 0.10, (0.0, 0.16, -0.28), gold, rot=(90, 0, 0), segs=3),
    ])
    assemble("shieldTrim", handR, [
        box("trimT", (0.66, 0.08, 0.06), (0.0, 0.21, 0.52), steel, bev=0.02),
        box("crossV", (0.07, 0.08, 0.80), (0.0, 0.21, 0.10), steel, bev=0.02),
        box("crossH", (0.50, 0.08, 0.07), (0.0, 0.21, 0.18), steel, bev=0.02),
        sphere("boss", 0.09, (0.0, 0.26, 0.14), steel, subdiv=2),
    ])

    # === ANIMATIONS =========================================================================
    # Every clip starts NEUTRAL at frame 1 so the exporter's rest sample is upright.

    # walk: a four-beat TROT. Diagonal pairs (frontL+backR / frontR+backL) swing together and
    # opposite the other diagonal; the body gives a two-beat vertical bob (a small pitch about
    # the planted hind hooves) + the head bobs and the tail sways.
    walk = bpy.data.actions.new("walk")
    for f, a in [(1, 24), (13, -24), (25, 24)]:
        key_rot(frontL, walk, f, (a, 0, 0));  key_rot(backR, walk, f, (a, 0, 0))
        key_rot(frontR, walk, f, (-a, 0, 0)); key_rot(backL, walk, f, (-a, 0, 0))
    for f, a in [(1, 0), (7, 2.5), (13, 0), (19, 2.5), (25, 0)]:   # gait bob
        key_rot(body, walk, f, (a, 0, 0))
    for f, a in [(1, 0), (13, -4), (25, 0)]:
        key_rot(head, walk, f, (a, 0, 0))
    for f, a in [(1, 0), (13, 10), (25, 0)]:
        key_rot(tail, walk, f, (0, a, 0))

    # idle: heavy breathing weight-shift (a tiny body pitch about the hooves, hooves stay
    # planted), a slow head bob, an occasional tail flick. Alive but grounded.
    idle = bpy.data.actions.new("idle")
    for f, a in [(1, 0.0), (31, 1.8), (61, 0.0)]:
        key_rot(body, idle, f, (a, 0, 0))
    for f, a, y in [(1, 0, 0), (31, -3, 2), (61, 0, 0)]:
        key_rot(head, idle, f, (a, y, 0))
    for f, a in [(1, 0), (20, 14), (34, -8), (61, 0)]:   # tail flick
        key_rot(tail, idle, f, (0, a, 0))

    # punch = REAR-AND-SLAM with a downward SPEAR thrust. The whole rig pitches up onto the
    # hind legs (front legs paw the air), the knight cocks the spear back, then the body SLAMS
    # forward/down and the spear drives down ahead. Full-body clip rooted on `body`.
    punch = bpy.data.actions.new("punch")
    # body: rear up (+pitch), hold, then slam through to a forward drive, recover.
    for f, a in [(1, 0), (8, 46), (15, 50), (21, -30), (25, -8), (32, 0)]:
        key_rot(body, punch, f, (a, 0, 0))
    # front legs tuck / paw the air on the rear, extend down on the slam.
    for f, a in [(1, 0), (8, 58), (15, 64), (21, -8), (32, 0)]:
        key_rot(frontL, punch, f, (a, 0, 0))
    for f, a in [(1, 0), (8, 46), (15, 54), (21, -8), (32, 0)]:
        key_rot(frontR, punch, f, (a, 0, 0))
    # spear arm: cock back at the rear (+X raises it further), then DRIVE down-forward (-X) on
    # the slam so the point stabs down ahead, then recover.
    for f, a in [(1, 0), (8, 26), (15, 34), (21, -118), (25, -128), (32, 0)]:
        key_rot(armL, punch, f, (a, 0, 0))
    key_rot(handL, punch, 1, (0, 0, 0)); key_rot(handL, punch, 21, (-22, 0, 0)); key_rot(handL, punch, 32, (0, 0, 0))
    # shield arm braces forward through the slam.
    for f, a in [(1, 0), (8, -10), (21, 18), (32, 0)]:
        key_rot(armR, punch, f, (a, 0, 0))
    # tail streams up on the rear.
    for f, a in [(1, 0), (10, -26), (21, 6), (32, 0)]:
        key_rot(tail, punch, f, (a, 0, 0))
    # head tosses up on the rear.
    for f, a in [(1, 0), (10, -16), (21, 8), (32, 0)]:
        key_rot(head, punch, f, (a, 0, 0))

    # --- Stash every clip on the bones it animates (unique track per clip/obj) so all export. -
    anim_bones = (body, head, frontL, frontR, backL, backR, tail, rider, armL, armR, handL, handR)
    for obj in anim_bones:
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(frontL, walk); stash(frontR, walk); stash(backL, walk); stash(backR, walk)
    stash(body, walk); stash(head, walk); stash(tail, walk)
    stash(body, idle); stash(head, idle); stash(tail, idle)
    stash(body, punch); stash(frontL, punch); stash(frontR, punch)
    stash(armL, punch); stash(handL, punch); stash(armR, punch); stash(tail, punch); stash(head, punch)

    # Reset every bone to its NEUTRAL rest so the EXPORTED rest pose stands upright.
    for obj in anim_bones:
        obj.rotation_euler = (0, 0, 0)
    bpy.context.scene.frame_set(1)

    out = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "mounted_knight.glb"))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_mounted_knight] wrote", out)


if __name__ == "__main__":
    main()
