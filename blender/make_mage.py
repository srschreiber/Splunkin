"""
Procedurally build a MAGE caster and export assets/models/mage.glb.

Re-skins the "ranged" enemy: same shoot behavior, new look. A hooded, robed wizard
that holds a staff (right hand) and casts a glowing projectile from its orb.

KEY ENGINE FACTS (see docs/blender-model-scripting.md):
  * No GPU skinning. Each glTF *primitive* is a rigid "part" pinned to the NODE that holds
    the mesh, drawn at placement * partNodeWorld. So the rig is a hierarchy of EMPTIES and
    meshes are parented to those bones.
  * A glTF primitive == one (mesh, material) pair. So ALL same-material geometry parented to
    one bone collapses into ONE primitive == ONE draw call, no matter how many vertices.
  * Clips are routed by name: idle / walk / punch (others ignored). `idle` is the looping
    base layer played when standing still; walk replaces it when moving; punch = the cast.

RIG (more bones than the old box version, for softer motion):
  body(waist) -> chest -> head, armL/armR(-> handL/handR)
  body -> legL, legR
  body -> hem            (lower-robe bone that sways independently of the waist)
The CHEST bone lets the upper body breathe in idle without lifting the planted hem/feet;
the HEM bone lets the skirt of the robe sway while walking/idling. The staff stays socketed
to handR so it inherits the cast motion. Existing names (body/head/armL/armR/handL/handR/
legL/legR) are kept; chest + hem are ADDED.

DESIGN: rounded smooth-shaded primitives (tapered cylinders / icospheres) -> a flowing robe,
rounded hood, smooth staff. Same-material geometry on a bone joins into ONE primitive, so the
robe folds, sleeve cuffs, hood rim, belt, mantle, staff finials and gem are all "free" detail.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_mage.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

ROBE  = (0.20, 0.26, 0.68, 1.0)   # blue robe
DROBE = (0.12, 0.15, 0.42, 1.0)   # darker blue trim / belt / cuffs
HAT   = (0.16, 0.20, 0.58, 1.0)   # hood + pointy hat cloth
STAR  = (0.98, 0.85, 0.25, 1.0)   # gold star / staff finials
SKIN  = (0.90, 0.74, 0.62, 1.0)
WHITE = (0.93, 0.93, 0.96, 1.0)   # beard
BROWN = (0.42, 0.27, 0.13, 1.0)   # staff shaft
ORB   = (0.35, 0.78, 1.00, 1.0)   # glowing staff orb (emissive)
GLOW  = (0.55, 0.88, 1.00, 1.0)   # glowing eyes (emissive)
BLACK = (0.04, 0.04, 0.05, 1.0)

CHEST_Z = 0.36   # chest bone height above the waist
HEM_Z   = -0.28  # hem (skirt) bone height below the waist


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

def bone_cyl(name, r_bot, r_top, depth, loc, mat_, rot=None, segs=16):
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
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.location = Vector(loc)
    obj.keyframe_insert(data_path="location", frame=frame)


def main():
    reset_scene()
    robe  = mat("m_robe", ROBE)
    drobe = mat("m_trim", DROBE)
    hat   = mat("m_hat",  HAT)
    star  = mat("m_star", STAR)
    skin  = mat("m_skin", SKIN)
    white = mat("m_beard", WHITE)
    brown = mat("m_staff", BROWN)
    orb   = mat("m_orb", ORB, emissive=True)
    glow  = mat("m_eye", GLOW, emissive=True)

    # --- Rig (empties). FRONT faces +Y. Existing names kept; chest + hem added. ---
    body  = make_empty("body",  None, (0, 0, 0.0))
    chest = make_empty("chest", body, (0, 0, CHEST_Z))
    head  = make_empty("head",  chest, (0, 0, 0.46))    # world ~0.82
    armL  = make_empty("armL",  chest, (0.24, 0, 0.32)) # world ~0.68
    armR  = make_empty("armR",  chest, (-0.24, 0, 0.32))
    handL = make_empty("handL", armL, (0, 0, -0.40))    # world ~0.28
    handR = make_empty("handR", armR, (0, 0, -0.40))
    legL  = make_empty("legL",  body, (0.11, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.11, 0, 0.0))
    hem   = make_empty("hem",   body, (0, 0, HEM_Z))

    # --- ROBE SKIRT (hem bone): a flared, tapering cylinder reaching the floor, with
    # vertical fold ridges. All robe-colored -> one primitive. Authored relative to hem. ---
    skirt = [
        bone_cyl("skirt", 0.37, 0.21, 0.94, (0, 0.0, -0.23), robe, segs=24),
        bone_cyl("skirtIn", 0.30, 0.20, 0.70, (0, 0.0, -0.10), robe, segs=20),
    ]
    for k in range(7):                       # fold ridges around the front/sides
        ang = math.radians(-120 + k * 40)
        fr = 0.345
        skirt.append(bone_cyl("fold%d" % k, 0.035, 0.022, 0.86,
                              (fr * math.sin(ang), fr * math.cos(ang), -0.27),
                              robe, segs=8))
    assemble("skirt", hem, skirt)
    assemble("hemtrim", hem, [
        bone_cyl("hemband", 0.40, 0.40, 0.07, (0, 0, -0.69), drobe, segs=24),
    ])

    # --- TORSO ROBE (chest bone): tapered body widening to the shoulders; breathes in idle. ---
    assemble("torso", chest, [
        bone_cyl("torso", 0.21, 0.245, 0.72, (0, 0.0, -0.02), robe, segs=20),
        sphere("chestM", 0.20, (0, 0.04, 0.10), robe, scale=(1.0, 0.85, 0.8), subdiv=2),
    ])
    # belt + shoulder mantle (dark trim, chest bone) -> one primitive.
    assemble("trim", chest, [
        bone_cyl("belt", 0.255, 0.255, 0.075, (0, 0, -0.30), drobe, segs=24),
        bone_cyl("buckle", 0.05, 0.05, 0.03, (0, 0.245, -0.30), drobe, rot=(90, 0, 0), segs=10),
        bone_cyl("mantle", 0.30, 0.20, 0.16, (0, 0, 0.30), drobe, segs=24),
    ])

    # --- HINT OF FEET under the hem (leg bones). ---
    for side, leg in (("L", legL), ("R", legR)):
        assemble("foot%s" % side, leg, [
            box("foot%s" % side, (0.13, 0.20, 0.10), (0, 0.04, -0.86), drobe, bev=0.03),
        ])

    # --- HEAD: face, glowing eyes, flowing beard. ---
    assemble("face", head, [
        sphere("face", 0.135, (0, 0.02, 0.0), skin, scale=(1.0, 0.95, 1.08), subdiv=3),
        sphere("nose", 0.03, (0, 0.15, -0.02), skin, subdiv=1),
    ])
    assemble("eyes", head, [
        sphere("eyeL", 0.026, (0.055, 0.125, 0.03), glow, subdiv=1),
        sphere("eyeR", 0.026, (-0.055, 0.125, 0.03), glow, subdiv=1),
    ])
    beard = [
        bone_cyl("beard", 0.055, 0.135, 0.36, (0, 0.085, -0.21), white, rot=(6, 0, 0), segs=14),
        sphere("stache", 0.05, (0, 0.13, -0.05), white, scale=(1.6, 1.0, 0.7), subdiv=2),
        sphere("beardTip", 0.04, (0, 0.075, -0.40), white, subdiv=2),
    ]
    assemble("beard", head, beard)

    # --- HOOD + POINTY HAT (hat cloth, head bone) -> one primitive. Hood cowls the crown
    # and frames the face; a tall tapered cone (drooping forward) sits on top. ---
    hood = [
        sphere("hood", 0.185, (0, -0.03, 0.045), hat, scale=(1.12, 1.06, 1.12), subdiv=3),
        sphere("cowl", 0.165, (0, 0.05, 0.0), hat, scale=(1.18, 1.0, 1.2), subdiv=2),
        bone_cyl("brim", 0.27, 0.23, 0.06, (0, 0.0, 0.17), hat, segs=20),
        bone_cyl("cone1", 0.20, 0.11, 0.26, (0, 0.02, 0.31), hat, segs=18),
        bone_cyl("cone2", 0.11, 0.02, 0.34, (0, 0.07, 0.57), hat, rot=(-12, 0, 0), segs=16),
        sphere("tipKnob", 0.03, (0, 0.16, 0.70), hat, subdiv=2),
    ]
    assemble("hood", head, hood)
    # hood rim (dark trim) framing the face opening.
    assemble("hoodrim", head, [
        bone_cyl("rim", 0.17, 0.17, 0.05, (0, 0.05, 0.03), drobe, rot=(72, 0, 0), segs=20),
    ])
    # gold star on the hat front + tip gem.
    assemble("star", head, [
        box("star", (0.07, 0.02, 0.07), (0, 0.165, 0.34), star, rot=(0, 45, 0), bev=0.005),
        sphere("tipGem", 0.028, (0, 0.17, 0.72), star, subdiv=1),
    ])

    # --- SLEEVES (arm bones): bell sleeve + shoulder cap, robe-colored. ---
    for side, arm in (("L", armL), ("R", armR)):
        assemble("sleeve%s" % side, arm, [
            sphere("shoulder%s" % side, 0.088, (0, 0, -0.012), robe, subdiv=2),
            bone_cyl("sleeve%s" % side, 0.13, 0.095, 0.40, (0, 0.0, -0.20), robe, segs=14),
        ])
        assemble("cuff%s" % side, arm, [   # dark cuff at the wrist
            bone_cyl("cuff%s" % side, 0.115, 0.10, 0.07, (0, 0.0, -0.39), drobe, segs=14),
        ])

    # --- HANDS (hand bones): skin spheres. ---
    for side, hand in (("L", handL), ("R", handR)):
        assemble("hand%s" % side, hand, [
            sphere("hand%s" % side, 0.07, (0, 0.0, -0.03), skin, scale=(1.0, 1.1, 1.0), subdiv=2),
        ])

    # --- STAFF (right hand): smooth shaft + finials, with a glowing orb cradled at the top. ---
    assemble("staff", handR, [
        bone_cyl("shaft", 0.03, 0.034, 1.5, (0, 0.05, 0.28), brown, segs=14),
        sphere("knob", 0.05, (0, 0.05, -0.46), brown, subdiv=2),          # butt of the staff
        bone_cyl("collar", 0.055, 0.055, 0.05, (0, 0.05, 0.92), brown, segs=12),
        # three claws cradling the orb
        bone_cyl("clawA", 0.018, 0.008, 0.16, (0.05, 0.05, 1.0), brown, rot=(0, -28, 0), segs=6),
        bone_cyl("clawB", 0.018, 0.008, 0.16, (-0.025, 0.095, 1.0), brown, rot=(28, 14, 0), segs=6),
        bone_cyl("clawC", 0.018, 0.008, 0.16, (-0.025, 0.005, 1.0), brown, rot=(-28, 14, 0), segs=6),
    ])
    assemble("orb", handR, [
        sphere("orb", 0.085, (0, 0.05, 1.06), orb, subdiv=3),
        sphere("spark", 0.03, (0.06, 0.10, 1.14), orb, subdiv=1),   # tiny cast spark
    ])

    # --- ANIMATIONS ---
    # walk: gentle glide (robe hides big leg motion); arms counter-swing, staff arm steady,
    # hem and chest sway/bob for life.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 16), (13, -16), (25, 16)]:
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a, 0, 0))
        key_rot(armR, walk, frame, (a * 0.35, 0, 0))   # keep the staff fairly upright
    for frame, a in [(1, 4), (13, -4), (25, 4)]:
        key_rot(hem, walk, frame, (a, 0, 0))           # skirt sways with the stride
    for frame, a in [(1, 1.5), (13, -1.5), (25, 1.5)]:
        key_rot(chest, walk, frame, (0, 0, a))         # subtle torso counter-rotation

    # idle: alive standing pose — chest breathing (lift + lean), slow head turn, arms/sleeves
    # sway, staff sways gently, hem drifts.
    idle = bpy.data.actions.new("idle")
    for frame, rise, lean in [(1, 0.0, 0.0), (31, 0.022, 1.6), (61, 0.0, 0.0)]:
        key_loc(chest, idle, frame, (0, 0, CHEST_Z + rise))
        key_rot(chest, idle, frame, (lean, 0, 0))
    for frame, ang in [(1, (0, 0, 0)), (21, (-2, 0, 6)), (41, (-1, 0, -5)), (61, (0, 0, 0))]:
        key_rot(head, idle, frame, ang)
    for frame, ax, az in [(1, 2, 4), (31, 5, 7), (61, 2, 4)]:
        key_rot(armL, idle, frame, (ax, 0, az))
        key_rot(armR, idle, frame, (ax, 0, -az))
    for frame, a in [(1, 0), (31, 2.5), (61, 0)]:
        key_rot(handR, idle, frame, (a, 0, a))         # staff tip sways
    for frame, a in [(1, 1.5), (31, -1.5), (61, 1.5)]:
        key_rot(hem, idle, frame, (a, 0, 0))           # robe hem drifts

    # punch == cast: wind the staff arm back then thrust forward as the orb sparks; the
    # other arm + chest lean into the cast.
    punch = bpy.data.actions.new("punch")
    for frame, a in [(1, 0), (6, -55), (12, 25), (18, 0)]:
        key_rot(armR, punch, frame, (a, 0, 0))
    for frame, a in [(1, 0), (6, 10), (12, -18), (18, 0)]:
        key_rot(armL, punch, frame, (a, 0, 0))
    for frame, a in [(1, 0), (12, 6), (18, 0)]:
        key_rot(chest, punch, frame, (a, 0, 0))

    # --- Park each action onto NLA so all clips export; then reset to a neutral rest pose. ---
    anim_objs = (body, chest, head, armL, armR, handL, handR, legL, legR, hem)
    for obj in anim_objs:
        if obj.animation_data: obj.animation_data.action = None

    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)

    for o in (legL, legR, armL, armR, hem, chest): stash(o, walk)
    for o in (chest, head, armL, armR, handR, hem): stash(o, idle)
    for o in (armR, armL, chest): stash(o, punch)

    # Reset rest transforms so the standing pose is neutral (keyframing leaves the last pose).
    for obj in anim_objs:
        obj.rotation_euler = (0, 0, 0)
    chest.location = Vector((0, 0, CHEST_Z))
    hem.location = Vector((0, 0, HEM_Z))

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "mage.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_mage] wrote", out_path)


if __name__ == "__main__":
    main()
