"""
Build a small impish GNOME mob and export assets/models/gnome.glb. It re-skins the
"flamethrower" enemy: same flame-cone behavior, new look. See docs/blender-model-scripting.md.

This is a graphics revamp of the old box-gnome: rounded, SMOOTH-shaded primitives so it
reads cute/menacing rather than blocky. Built the same way as the gold-standard skeleton
(blender/make_skeleton.py): a hierarchy of EMPTY bones with a separate mesh rigidly parented
to each bone, and ALL same-material geometry on a bone JOINED into one object so it collapses
to ONE glTF primitive == ONE draw call. So despite a big nose, ears, fingers, belt, buckle,
boots and a stacked hat, the whole gnome is ~20 parts.

KEY ENGINE FACTS:
  * No GPU skinning. Each glTF primitive is a rigid part pinned to the NODE holding the mesh.
  * Clips routed by name: idle / walk / punch (others ignored). `idle` is the looping base
    layer played when standing still; walk replaces it when moving.

RIG (kept names body/head/armL/armR/handL/handR/legL/legR; ADDED chest/kneeL/kneeR/hattip):
  body(waist) -> chest -> head -> hattip, armL/armR(-> handL/handR)
  body -> legL(-> kneeL), legR(-> kneeR)
The CHEST bone lets the upper body bob/breathe in idle without lifting the planted feet; the
KNEE bones bend the stubby legs through the walk; HATTIP wobbles the droopy hat tip.

Z-up; front faces +Y; waist at origin; feet near Z=-1.0 so the engine's MODEL_FOOT_LIFT
grounds it. Stout, big-headed, short-legged so it reads as a gnome.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_gnome.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

GREEN  = (0.13, 0.52, 0.20, 1.0)   # tunic / sleeves
DGREEN = (0.09, 0.36, 0.14, 1.0)   # legs / boots
RED    = (0.85, 0.12, 0.12, 1.0)   # pointy hat
SKIN   = (0.86, 0.62, 0.48, 1.0)   # face / hands / big nose / ears
WHITE  = (0.93, 0.93, 0.96, 1.0)   # beard / mustache / brows
BROWN  = (0.45, 0.29, 0.14, 1.0)   # staff / belt
GOLD   = (0.95, 0.78, 0.20, 1.0)   # belt buckle / staff knob
AMBER  = (0.99, 0.62, 0.12, 1.0)   # glowing impish eyes
CHEST_Z = 0.20   # chest bone height above the waist (low + stout)


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
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = 3.5
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

def box(name, size, loc, mat_, rot=None, bev=0.02, smooth=False):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), smooth)


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
    """Keyframe a node's LOCAL translation. Must include the bone's base offset (clips set
    the node transform absolutely, so omitting it would snap the bone to the origin)."""
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.location = Vector(loc)
    obj.keyframe_insert(data_path="location", frame=frame)


def main():
    reset_scene()
    green  = mat("g_tunic", GREEN)
    dgreen = mat("g_legs",  DGREEN)
    red    = mat("g_hat",   RED)
    skin   = mat("g_skin",  SKIN)
    white  = mat("g_beard", WHITE)
    brown  = mat("g_staff", BROWN)
    gold   = mat("g_gold",  GOLD)
    amber  = mat("g_eye",   AMBER, emissive=True)

    # --- Rig (empties). FRONT faces +Y. Stout + big-headed + short-legged. ---
    body   = make_empty("body",   None,  (0, 0, 0.0))
    chest  = make_empty("chest",  body,  (0, 0, CHEST_Z))
    head   = make_empty("head",   chest, (0, 0, 0.34))
    hattip = make_empty("hattip", head,  (0, 0.05, 0.34))
    armL   = make_empty("armL",   chest, (0.26, 0, 0.06))
    armR   = make_empty("armR",   chest, (-0.26, 0, 0.06))
    handL  = make_empty("handL",  armL,  (0, 0, -0.32))
    handR  = make_empty("handR",  armR,  (0, 0, -0.32))
    legL   = make_empty("legL",   body,  (0.12, 0, -0.06))
    legR   = make_empty("legR",   body,  (-0.12, 0, -0.06))
    kneeL  = make_empty("kneeL",  legL,  (0, 0, -0.36))
    kneeR  = make_empty("kneeR",  legR,  (0, 0, -0.36))

    # --- HEAD: round skin head, bulbous nose, pointy ears, cheeks (one skin primitive);
    # glowing amber eyes; white beard/mustache/brows; lower red hat (brim + first cone). ---
    assemble("face", head, [
        sphere("skull",  0.165, (0, 0.01, 0.04),  skin, scale=(1.0, 1.02, 1.04), subdiv=3),
        sphere("nose",   0.062, (0, 0.18, -0.01),  skin, scale=(0.9, 1.25, 0.95), subdiv=2),
        sphere("noseTip",0.04,  (0, 0.215, -0.03), skin, subdiv=2),
        sphere("cheekL", 0.06,  (0.10, 0.115, -0.04), skin, subdiv=2),
        sphere("cheekR", 0.06,  (-0.10, 0.115, -0.04), skin, subdiv=2),
        bone_cyl("earL", 0.055, 0.005, 0.13, (0.165, 0.0, 0.05), skin, rot=(0, 60, 0), segs=8),
        bone_cyl("earR", 0.055, 0.005, 0.13, (-0.165, 0.0, 0.05), skin, rot=(0, -60, 0), segs=8),
    ])
    assemble("eyes", head, [
        sphere("eyeL", 0.032, (0.075, 0.135, 0.08), amber, subdiv=2),
        sphere("eyeR", 0.032, (-0.075, 0.135, 0.08), amber, subdiv=2),
    ])
    assemble("whiskers", head, [
        # full beard hanging over the chest, fat mustache, bushy brows
        sphere("beard1", 0.17, (0, 0.085, -0.16), white, scale=(1.0, 0.7, 1.05), subdiv=2),
        sphere("beard2", 0.12, (0, 0.075, -0.32), white, scale=(1.0, 0.65, 1.1), subdiv=2),
        sphere("beard3", 0.075, (0, 0.06, -0.45), white, scale=(0.9, 0.6, 1.1), subdiv=2),
        bone_cyl("mustL", 0.04, 0.012, 0.13, (0.06, 0.165, 0.0), white, rot=(70, 0, 18), segs=7),
        bone_cyl("mustR", 0.04, 0.012, 0.13, (-0.06, 0.165, 0.0), white, rot=(70, 0, -18), segs=7),
        sphere("browL", 0.04, (0.08, 0.13, 0.13), white, scale=(1.5, 0.7, 0.7), subdiv=1),
        sphere("browR", 0.04, (-0.08, 0.13, 0.13), white, scale=(1.5, 0.7, 0.7), subdiv=1),
    ])
    assemble("hatlow", head, [
        bone_cyl("brim", 0.255, 0.235, 0.05, (0, 0.0, 0.165), red, segs=20),
        bone_cyl("hat1", 0.20, 0.145, 0.18, (0, 0.0, 0.28), red, segs=18),
    ])

    # --- HATTIP bone: upper cone + drooping tip (wobbles in idle). Origin at the cone base. ---
    assemble("hatcone", hattip, [
        bone_cyl("hat2", 0.135, 0.075, 0.20, (0, 0.0, 0.06), red, segs=14),
        bone_cyl("hat3", 0.07, 0.022, 0.18, (0, 0.03, 0.22), red, rot=(14, 0, 0), segs=12),
        sphere("tipball", 0.045, (0, 0.085, 0.33), gold, subdiv=2),
    ])

    # --- CHEST: rounded green tunic torso + shoulders + a white collar tuft (rides the
    # chest bone so it bobs/breathes in idle without lifting the feet). ---
    assemble("torso", chest, [
        sphere("belly", 0.30, (0, 0.0, -0.12), green, scale=(1.0, 0.92, 1.15), subdiv=3),
        sphere("shoulderL", 0.085, (0.225, 0, 0.04), green, subdiv=2),
        sphere("shoulderR", 0.085, (-0.225, 0, 0.04), green, subdiv=2),
    ])
    assemble("collar", chest, [
        sphere("collar", 0.16, (0, 0.07, 0.12), white, scale=(1.0, 0.6, 0.5), subdiv=2),
    ])

    # --- BODY (waist): lower tunic skirt, brown belt + gold buckle (stay put while the
    # chest breathes above). ---
    assemble("skirt", body, [
        bone_cyl("skirt", 0.30, 0.26, 0.22, (0, 0, -0.05), green, segs=20),
    ])
    assemble("belt", body, [
        bone_cyl("belt", 0.285, 0.285, 0.07, (0, 0, 0.06), brown, segs=20),
    ])
    assemble("buckle", body, [
        box("buckle", (0.10, 0.05, 0.07), (0, 0.27, 0.06), gold, bev=0.012),
    ])

    # --- ARMS: tapered green sleeve + shoulder cap on each arm bone. ---
    def sleeve(side, bonenode):
        assemble("sleeve%s" % side, bonenode, [
            sphere("scap%s" % side, 0.085, (0, 0, 0.0), green, subdiv=2),
            bone_cyl("uarm%s" % side, 0.075, 0.06, 0.32, (0, 0, -0.16), green, segs=10),
            sphere("cuff%s" % side, 0.07, (0, 0, -0.30), green, subdiv=2),
        ])
    sleeve("L", armL); sleeve("R", armR)

    # --- HANDS: rounded skin palm + stubby fingers + thumb (one skin primitive). ---
    def hand(side, bonenode):
        parts = [
            sphere("palm%s" % side, 0.075, (0, 0, -0.04), skin, scale=(1.0, 0.85, 1.1), subdiv=2),
        ]
        for k in range(3):
            x = -0.035 + k * 0.035
            parts.append(bone_cyl("fing%s%d" % (side, k), 0.02, 0.016, 0.09, (x, 0.02, -0.12), skin, segs=6))
        parts.append(bone_cyl("thumb%s" % side, 0.022, 0.018, 0.07, (0.06, 0.0, -0.06), skin, rot=(0, 60, 0), segs=6))
        assemble("hand%s" % side, bonenode, parts)
    hand("L", handL); hand("R", handR)

    # --- STAFF: tall brown stick rising past the head, gnarled gold knob on top (right hand). ---
    assemble("staff", handR, [
        bone_cyl("shaft", 0.045, 0.055, 1.15, (0, 0.07, 0.42), brown, segs=10),
        sphere("knob", 0.10, (0, 0.07, 1.02), gold, subdiv=2),
        sphere("knob2", 0.055, (0, 0.07, 0.90), brown, subdiv=2),
    ])

    # --- LEGS: tapered dgreen thigh on the leg bone; shin + chunky boot on the knee bone
    # so the knee bends through the walk. Boot toe points +Y; sole near Z=-1.0. ---
    def thigh(side, bonenode):
        assemble("thigh%s" % side, bonenode, [
            sphere("hip%s" % side, 0.10, (0, 0, 0.0), dgreen, subdiv=2),
            bone_cyl("femur%s" % side, 0.085, 0.07, 0.36, (0, 0, -0.18), dgreen, segs=10),
            sphere("knee%s" % side, 0.075, (0, 0, -0.36), dgreen, subdiv=2),
        ])
    def boot(side, bonenode):
        assemble("boot%s" % side, bonenode, [
            bone_cyl("shin%s" % side, 0.07, 0.075, 0.40, (0, 0, -0.20), dgreen, segs=10),
            sphere("ankle%s" % side, 0.08, (0, 0.0, -0.40), dgreen, subdiv=2),
            # curled-toe boot: heel block + forward toe ball, slightly oversized + comical
            box("sole%s" % side, (0.17, 0.30, 0.10), (0, 0.07, -0.52), dgreen, bev=0.04, smooth=True),
            sphere("toe%s" % side, 0.085, (0, 0.22, -0.46), dgreen, scale=(1.0, 1.1, 1.0), subdiv=2),
        ])
    thigh("L", legL); boot("L", kneeL)
    thigh("R", legR); boot("R", kneeR)

    # --- Animations ---
    # walk: short stubby stride. Legs swing opposite, knees flex on the back-swing, left arm
    # swings free while the right keeps the staff a bit steadier; a little shoulder counter-sway.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 24), (13, -24), (25, 24)]:
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a * 0.7, 0, 0))
        key_rot(armR, walk, frame, (a * 0.3, 0, 0))
    for frame, kl, kr in [(1, 6, 42), (7, 32, 22), (13, 42, 6), (19, 22, 32), (25, 6, 42)]:
        key_rot(kneeL, walk, frame, (kl, 0, 0))
        key_rot(kneeR, walk, frame, (kr, 0, 0))
    for frame, r in [(1, 4), (13, -4), (25, 4)]:
        key_rot(chest, walk, frame, (0, 0, r))   # waddle: torso twist

    # idle: a lively impish fidget. Chest bobs/breathes (feet stay planted), head tilts and
    # cranes, the hat tip wobbles, hands twitch, shoulders shrug. 80-frame loop.
    idle = bpy.data.actions.new("idle")
    for frame, rise, lean in [(1, 0.0, 0.0), (20, 0.03, 3.0), (40, 0.05, -1.0),
                              (60, 0.02, 4.0), (80, 0.0, 0.0)]:
        key_loc(chest, idle, frame, (0, 0, CHEST_Z + rise))
        key_rot(chest, idle, frame, (lean, 0, 0))
    for frame, ax, az in [(1, 0, 0), (20, 4, -6), (40, -3, 5), (60, 5, -3), (80, 0, 0)]:
        key_rot(head, idle, frame, (ax, 0, az))   # curious head tilt + crane
    for frame, ax, ay in [(1, 0, 0), (20, 8, 6), (40, -5, -4), (60, 10, 8), (80, 0, 0)]:
        key_rot(hattip, idle, frame, (ax, ay, 0))  # floppy hat-tip wobble
    for frame, a, z in [(1, 5, 4), (20, 12, 9), (40, 3, 2), (60, 14, 10), (80, 5, 4)]:
        key_rot(armL, idle, frame, (a, 0, z))      # left arm sways out
        key_rot(armR, idle, frame, (a, 0, -z * 0.5))
    for frame, a in [(1, 0), (20, -18), (40, 6), (60, -22), (80, 0)]:
        key_rot(handL, idle, frame, (a, 0, 0))     # twitchy fingers/wrist

    # punch: the gnome "casts" — left arm winds back then snaps forward/down, wrist flicks.
    punch = bpy.data.actions.new("punch")
    for frame, a in [(1, 0), (5, -45), (10, 100), (16, 0)]:
        key_rot(armL, punch, frame, (a, 0, 0))
    key_rot(handL, punch, 10, (30, 0, 0))
    key_rot(handL, punch, 16, (0, 0, 0))

    # --- Reset every animated bone to its REST pose before export (else the exporter bakes
    # the last keyframe as the node's rest transform and the gnome stands permanently posed). ---
    for obj in (chest, head, hattip, legL, legR, armL, armR, handL, kneeL, kneeR):
        if obj.animation_data: obj.animation_data.action = None
    chest.location = Vector((0, 0, CHEST_Z))
    for obj in (chest, head, hattip, legL, legR, armL, armR, handL, kneeL, kneeR):
        obj.rotation_euler = (0, 0, 0)

    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk); stash(armL, walk); stash(armR, walk)
    stash(kneeL, walk); stash(kneeR, walk); stash(chest, walk)
    stash(chest, idle); stash(head, idle); stash(hattip, idle)
    stash(armL, idle); stash(armR, idle); stash(handL, idle)
    stash(armL, punch); stash(handL, punch)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "gnome.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_gnome] wrote", out_path)


if __name__ == "__main__":
    main()
