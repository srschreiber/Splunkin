"""
Build a flying DEMON enemy (horned, winged, glowing) and export assets/models/demon.glb.
Behavior (engine): big + strong; hurls slow fireballs that EXPLODE on impact (rocket-launcher
style splash). It HOVERS/FLIES rather than standing still. See docs/blender-model-scripting.md.

KEY ENGINE FACTS (mirrored from make_skeleton.py, the gold-standard):
  * No GPU skinning. Each glTF *primitive* is a rigid "part" pinned to the NODE that holds the
    mesh, drawn at placement * partNodeWorld. So the rig is a hierarchy of EMPTIES and meshes
    are parented to those bones.
  * A glTF primitive == one (mesh, material) pair. So ALL same-material geometry parented to one
    bone collapses into ONE primitive == ONE draw call, no matter how many verts. -> add muscle,
    brow, claws, horn ridges and wing struts freely as long as they share a material.
  * Clips routed by name: idle / walk / punch (others ignored). `idle` is the looping base layer
    the engine plays when the creature holds station; walk replaces it when moving.

RIG (more bones than the old box version, for softer / less-rigid motion):
  body(waist) -> chest -> head -> jaw
  chest -> armL/armR(-> handL/handR), wingL/wingR(-> wingtipL/wingtipR)
  body -> legL/legR(-> kneeL/kneeR)
The CHEST bone lets the torso breathe/heave in idle without lifting the dangling legs; the
WING-TIP bones let the membranes whip with a lag instead of flapping as rigid planks; the KNEE
bones let the hooved legs dangle and kick under the hovering body; the JAW gapes for the cast.

DESIGN: rounded icospheres + tapered cylinders + SMOOTH shading for an organic, muscled, NOT
blocky silhouette; flat bevelled plates only for the wing membranes. Same-material detail joins
per bone, so the demon stays ~23 primitives despite muscle masses, brow, claws and wing struts.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_demon.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

RED  = (0.58, 0.12, 0.09, 1.0)   # demon hide
DARK = (0.26, 0.06, 0.05, 1.0)   # shadowed muscle / pelvis / wing struts
HORN = (0.09, 0.08, 0.10, 1.0)   # horns
FIRE = (1.00, 0.48, 0.10, 1.0)   # emissive glow (eyes, core, maw, hands)
WING = (0.30, 0.07, 0.08, 1.0)   # wing membrane
CHEST_Z = 0.42                   # chest bone height above the waist


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

def box(name, size, loc, mat_, rot=None, bev=0.012, scale=(1, 1, 1)):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, scale, False)


def assemble(name, bone, objs):
    """Bake each part's transform, JOIN all of them into one mesh, pin to the bone.
    NOTE: join collapses everything regardless of material, but the glTF exporter still emits
    one primitive *per material* on the joined mesh, so mixing N materials here costs N draw
    calls. Group same-material detail to keep the count down."""
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
    red  = mat("d_red",  RED)
    dark = mat("d_dark", DARK)
    horn = mat("d_horn", HORN)
    fire = mat("d_fire", FIRE, emissive=True)
    wing = mat("d_wing", WING)

    # --- Rig (empties). FRONT faces +Y. ---
    body  = make_empty("body",  None, (0, 0, 0.0))
    chest = make_empty("chest", body, (0, 0, CHEST_Z))
    head  = make_empty("head",  chest, (0, 0, 0.58))      # raised so the round chest doesn't bury the head
    jaw   = make_empty("jaw",   head, (0, -0.05, -0.05))  # hinge near the back of the skull
    armL  = make_empty("armL",  chest, (0.38, 0, 0.32))
    armR  = make_empty("armR",  chest, (-0.38, 0, 0.32))
    handL = make_empty("handL", armL, (0, 0, -0.52))
    handR = make_empty("handR", armR, (0, 0, -0.52))
    legL  = make_empty("legL",  body, (0.16, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.16, 0, 0.0))
    kneeL = make_empty("kneeL", legL, (0, 0, -0.52))
    kneeR = make_empty("kneeR", legR, (0, 0, -0.52))
    wingL = make_empty("wingL", chest, (0.20, -0.16, 0.20))   # roots on chest -> ride the heave
    wingR = make_empty("wingR", chest, (-0.20, -0.16, 0.20))
    wtipL = make_empty("wingtipL", wingL, (0.78, -0.10, 0.04))
    wtipR = make_empty("wingtipR", wingR, (-0.78, -0.10, 0.04))

    # --- TORSO on the chest bone: heaving ribcage, pecs, abdominal masses + a glowing fire core
    #     showing through the breastbone. All the muscle is `red` (1 prim); the core is `fire`. ---
    cz = lambda z: z - CHEST_Z
    assemble("torso", chest, [
        sphere("ribcage", 0.34, (0, 0.05, cz(0.52)), red, scale=(1.1, 0.86, 0.95), subdiv=3),   # single smooth chest (no separate pec bulbs), lowered so the head clears it
        sphere("delt L", 0.13, (0.34, 0.0, cz(0.70)), red, subdiv=2),
        sphere("deltR",  0.13, (-0.34, 0.0, cz(0.70)), red, subdiv=2),
        sphere("ab0",    0.20, (0, 0.06, cz(0.34)), red, scale=(1.0, 0.8, 0.95), subdiv=2),
        sphere("ab1",    0.16, (0, 0.07, cz(0.18)), red, scale=(1.0, 0.8, 0.9), subdiv=2),
        # trapezius ridge sweeping up to the neck
        box("trap", (0.30, 0.20, 0.12), (0, -0.06, cz(0.74)), red, bev=0.04),
    ])
    assemble("core", chest, [
        sphere("firecore", 0.10, (0, 0.20, cz(0.50)), fire, scale=(1.1, 0.7, 1.3), subdiv=2),
    ])

    # --- PELVIS + lower belly on the waist (body) so it doesn't heave with the chest. ---
    assemble("pelvis", body, [
        sphere("hipmass", 0.26, (0, 0.0, -0.06), dark, scale=(1.05, 0.85, 0.9), subdiv=2),
        sphere("groin",   0.14, (0, 0.04, -0.22), dark, subdiv=2),
        sphere("waist",   0.22, (0, 0.05, 0.06), red, scale=(1.0, 0.8, 0.85), subdiv=2),
    ])

    # --- HEAD on the head bone: rounded skull + heavy brow + cheeks (red), backswept horns
    #     (horn), glowing eyes (fire). Detail is free within each material. ---
    assemble("skull", head, [
        sphere("cranium", 0.20, (0, 0.0, 0.07), red, scale=(1.0, 1.05, 1.0), subdiv=3),
        sphere("snout",   0.13, (0, 0.16, -0.02), red, scale=(1.0, 1.1, 0.85), subdiv=2),
        box("brow", (0.34, 0.10, 0.06), (0, 0.14, 0.12), red, rot=(-12, 0, 0), bev=0.02),
        sphere("cheekL", 0.07, (0.14, 0.12, 0.0), red, subdiv=2),
        sphere("cheekR", 0.07, (-0.14, 0.12, 0.0), red, subdiv=2),
        bone_cyl("neck", 0.13, 0.15, 0.14, (0, -0.04, -0.16), red, segs=12),
        # brow ridge spikes
        bone_cyl("browSpkL", 0.02, 0.0, 0.10, (0.10, 0.18, 0.17), red, rot=(-60, 0, 0), segs=6),
        bone_cyl("browSpkR", 0.02, 0.0, 0.10, (-0.10, 0.18, 0.17), red, rot=(-60, 0, 0), segs=6),
    ])
    assemble("horns", head, [
        # two-segment backswept horns per side, tapering to a point + a small ridge nub
        bone_cyl("hornL0", 0.055, 0.035, 0.26, (0.13, -0.02, 0.24), horn, rot=(28, 0, 12), segs=10),
        bone_cyl("hornL1", 0.035, 0.0,  0.24, (0.18, -0.16, 0.40), horn, rot=(55, 0, 18), segs=10),
        bone_cyl("hornR0", 0.055, 0.035, 0.26, (-0.13, -0.02, 0.24), horn, rot=(28, 0, -12), segs=10),
        bone_cyl("hornR1", 0.035, 0.0,  0.24, (-0.18, -0.16, 0.40), horn, rot=(55, 0, -18), segs=10),
    ])
    assemble("eyes", head, [
        sphere("eyeL", 0.045, (0.10, 0.18, 0.09), fire, scale=(1.1, 0.8, 0.7), subdiv=2),
        sphere("eyeR", 0.045, (-0.10, 0.18, 0.09), fire, scale=(1.1, 0.8, 0.7), subdiv=2),
    ])

    # --- JAW on the jaw bone (gapes for the cast): dark lower jaw + fangs, glowing maw inside. ---
    jaw_parts = [
        box("jawmesh", (0.26, 0.26, 0.10), (0, 0.16, -0.06), dark, bev=0.03),
        sphere("chin", 0.07, (0, 0.26, -0.08), dark, subdiv=2),
    ]
    for k, x in enumerate((-0.07, -0.025, 0.025, 0.07)):   # lower fangs (dark, joins with jaw)
        jaw_parts.append(bone_cyl("fang%d" % k, 0.018, 0.0, 0.07, (x, 0.24, -0.01), dark, rot=(180, 0, 0), segs=6))
    assemble("jawmass", jaw, jaw_parts)
    assemble("maw", jaw, [
        sphere("throat", 0.09, (0, 0.12, -0.02), fire, scale=(1.2, 1.0, 0.6), subdiv=2),
    ])

    # --- ARMS: shoulder + tapered, muscled upper arm + forearm (all red, 1 prim each). ---
    def arm(side, bonenode):
        assemble("arm%s" % side, bonenode, [
            sphere("shldr%s" % side, 0.118, (0, 0, 0.0), red, subdiv=2),
            bone_cyl("biceps%s" % side, 0.11, 0.085, 0.30, (0, 0, -0.17), red, segs=12),
            sphere("elbow%s" % side, 0.09, (0, 0, -0.34), red, subdiv=2),
            bone_cyl("forearm%s" % side, 0.085, 0.07, 0.18, (0, 0, -0.45), red, segs=12),
        ])
    arm("L", armL); arm("R", armR)

    # --- HANDS: glowing clawed fists where the fireballs gather (all fire-emissive, 1 prim). ---
    def hand(side, bonenode):
        parts = [sphere("fist%s" % side, 0.12, (0, 0.0, -0.02), fire, scale=(1.0, 1.1, 1.0), subdiv=2)]
        for k, x in enumerate((-0.06, -0.02, 0.02, 0.06)):   # curled claws
            parts.append(bone_cyl("claw%s%d" % (side, k), 0.022, 0.0, 0.13, (x, 0.10, -0.07),
                                  fire, rot=(115, 0, 0), segs=6))
        parts.append(bone_cyl("thumb%s" % side, 0.024, 0.0, 0.11, (0.09, 0.02, -0.04),
                              fire, rot=(70, 0, -40), segs=6))
        assemble("hand%s" % side, bonenode, parts)
    hand("L", handL); hand("R", handR)

    # --- LEGS: thigh on the leg bone; shin + cloven hoof on the knee bone (so it dangles/bends). ---
    def thigh(side, bonenode):
        assemble("thigh%s" % side, bonenode, [
            sphere("hip%s" % side, 0.15, (0, 0, 0.02), red, subdiv=2),
            bone_cyl("femur%s" % side, 0.13, 0.09, 0.46, (0, 0, -0.24), red, segs=12),
            sphere("knee%s" % side, 0.10, (0, 0, -0.50), red, subdiv=2),
        ])
    def shin(side, bonenode):
        parts = [
            bone_cyl("tibia%s" % side, 0.085, 0.06, 0.36, (0, -0.02, -0.20), dark, segs=12),
            sphere("ankle%s" % side, 0.07, (0, 0.0, -0.40), dark, subdiv=2),
            # cloven hoof: two toes pointing forward
            box("hoofA%s" % side, (0.07, 0.20, 0.12), (0.045, 0.06, -0.49), dark, bev=0.02),
            box("hoofB%s" % side, (0.07, 0.20, 0.12), (-0.045, 0.06, -0.49), dark, bev=0.02),
        ]
        assemble("shin%s" % side, bonenode, parts)
    thigh("L", legL); shin("L", kneeL)
    thigh("R", legR); shin("R", kneeR)

    # --- WINGS: a leathery membrane (wing material) ribbed by dark finger-struts. Inner half on
    #     the wing bone, outer half on the wing-tip bone so the membrane whips with a lag. ---
    def wing_inner(side, bonenode):
        s = 1.0 if side == "L" else -1.0
        parts = [
            # membrane plate, fanned back and tilted; flat bevelled plate is fine here
            box("memb%s" % side, (0.62, 0.66, 0.025), (s*0.34, -0.12, 0.0), wing,
                rot=(0, 0, -18*s), bev=0.02, scale=(1, 1, 1)),
        ]
        # leading-edge arm bone + two finger struts (all dark, join into 1 prim)
        parts.append(bone_cyl("leadbone%s" % side, 0.05, 0.03, 0.78, (s*0.36, 0.06, 0.06),
                              dark, rot=(0, 90*s, 0), segs=8))
        parts.append(bone_cyl("fingA%s" % side, 0.028, 0.012, 0.55, (s*0.30, -0.22, 0.0),
                              dark, rot=(0, 70*s, 0), segs=6))
        parts.append(bone_cyl("fingB%s" % side, 0.026, 0.012, 0.50, (s*0.22, -0.40, -0.02),
                              dark, rot=(0, 55*s, 0), segs=6))
        # we want membrane and struts as SEPARATE materials but joined per bone -> 2 prims here
        wmemb = [parts[0]]
        struts = parts[1:]
        assemble("wingmemb%s" % side, bonenode, wmemb)
        assemble("wingrib%s" % side, bonenode, struts)
    def wing_outer(side, bonenode):
        s = 1.0 if side == "L" else -1.0
        # outer membrane + a tip claw + trailing struts, all on the tip bone. Membrane (wing) +
        # struts (dark) = 2 prims.
        assemble("tipmemb%s" % side, bonenode, [
            box("tipmemb%s" % side, (0.50, 0.58, 0.022), (s*0.20, -0.20, -0.02), wing,
                rot=(0, 0, -26*s), bev=0.02),
        ])
        assemble("tiprib%s" % side, bonenode, [
            bone_cyl("tipbone%s" % side, 0.03, 0.0, 0.50, (s*0.24, 0.0, 0.02),
                     dark, rot=(0, 80*s, 0), segs=6),
            bone_cyl("tipfing%s" % side, 0.022, 0.01, 0.42, (s*0.14, -0.34, -0.04),
                     dark, rot=(0, 45*s, 0), segs=6),
            sphere("tipclaw%s" % side, 0.03, (s*0.46, 0.05, 0.06), horn, subdiv=1),
        ])
    wing_inner("L", wingL); wing_outer("L", wtipL)
    wing_inner("R", wingR); wing_outer("R", wtipR)

    # =============================== ANIMATIONS ===============================
    # idle (HOVER): the demon holds station in the air -> a slow steady wing flap, a buoyant
    # body bob, the chest heaving (breathing fire), wing-tips whipping with a lag, head sway,
    # and the legs dangling slightly. Lively, never static.
    idle = bpy.data.actions.new("idle")
    # whole-body buoyancy bob (root) -- rises as the wings push down
    for frame, z in [(1, 0.0), (16, 0.05), (31, 0.0), (46, 0.05), (61, 0.0)]:
        key_loc(body, idle, frame, (0, 0, z))
    # chest heave / breath (independent of the bob so the torso swells)
    for frame, rise, lean in [(1, 0.0, 0.0), (31, 0.03, -2.5), (61, 0.0, 0.0)]:
        key_loc(chest, idle, frame, (0, 0, CHEST_Z + rise))
        key_rot(chest, idle, frame, (lean, 0, 0))
    # wings: two slow flaps per loop (about Y). L and R mirror.
    for frame, w in [(1, 24), (16, -16), (31, 24), (46, -16), (61, 24)]:
        key_rot(wingL, idle, frame, (0, w, 6))
        key_rot(wingR, idle, frame, (0, -w, -6))
    # wing-tips whip with a ~5-frame lag and bigger throw -> membranes feel soft
    for frame, w in [(1, -10), (21, 22), (36, -10), (51, 22), (61, -10)]:
        key_rot(wtipL, idle, frame, (0, w, 0))
        key_rot(wtipR, idle, frame, (0, -w, 0))
    # head sway + slow breath through the jaw
    for frame, a, y in [(1, 2, 4), (31, -3, -4), (61, 2, 4)]:
        key_rot(head, idle, frame, (a, 0, y))
    for frame, j in [(1, 0), (31, 7), (61, 0)]:
        key_rot(jaw, idle, frame, (j, 0, 0))
    # arms sway, legs dangle (knees swing a touch under the hover)
    for frame, ax, az in [(1, 4, 5), (31, 8, 9), (61, 4, 5)]:
        key_rot(armL, idle, frame, (ax, 0, az))
        key_rot(armR, idle, frame, (ax, 0, -az))
    for frame, a in [(1, 6), (31, -6), (61, 6)]:
        key_rot(legL, idle, frame, (a, 0, 0))
        key_rot(legR, idle, frame, (-a, 0, 0))
        key_rot(kneeL, idle, frame, (a + 10, 0, 0))
        key_rot(kneeR, idle, frame, (-a + 10, 0, 0))

    # walk (FLY FORWARD): stronger driving wing-beats, body surge, legs trailing & kicking.
    walk = bpy.data.actions.new("walk")
    for frame, z in [(1, 0.0), (16, 0.08), (31, 0.0)]:
        key_loc(body, walk, frame, (0, 0, z))
    for frame, w in [(1, 40), (16, -28), (31, 40)]:
        key_rot(wingL, walk, frame, (0, w, 8))
        key_rot(wingR, walk, frame, (0, -w, -8))
    for frame, w in [(1, -18), (10, 34), (24, -18), (31, 6)]:    # tip lag/whip
        key_rot(wtipL, walk, frame, (0, w, 0))
        key_rot(wtipR, walk, frame, (0, -w, 0))
    for frame, a in [(1, 16), (16, -16), (31, 16)]:               # legs cycle/kick
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(kneeL, walk, frame, (30 - a, 0, 0))
        key_rot(kneeR, walk, frame, (30 + a, 0, 0))
    for frame, a in [(1, 6), (16, -6), (31, 6)]:                  # arms counter-swing
        key_rot(armL, walk, frame, (-a, 0, 6))
        key_rot(armR, walk, frame, (a, 0, -6))

    # punch = the CAST/YELL: rears back, jaw gapes (glowing maw), arm hurls the fireball, wings flare.
    punch = bpy.data.actions.new("punch")
    for frame, a in [(1, 0), (7, -70), (13, 30), (20, 0)]:
        key_rot(armL, punch, frame, (a, 0, 0))
    for frame, j in [(1, 0), (7, 14), (13, 58), (20, 0)]:
        key_rot(jaw, punch, frame, (j, 0, 0))
    for frame, h in [(1, 0), (7, -8), (13, -22), (20, 0)]:
        key_rot(head, punch, frame, (h, 0, 0))
    for frame, w in [(1, 18), (7, 48), (13, 30), (20, 18)]:       # wings flare open during the cast
        key_rot(wingL, punch, frame, (0, w, 6))
        key_rot(wingR, punch, frame, (0, -w, -6))

    # ---- Reset every animated bone to neutral, then stash each action on its own NLA track ----
    animated = (body, chest, head, jaw, armL, armR, legL, legR, kneeL, kneeR, wingL, wingR, wtipL, wtipR)
    for obj in animated:
        if obj.animation_data: obj.animation_data.action = None
        obj.rotation_euler = (0, 0, 0)
    body.location = Vector((0, 0, 0)); chest.location = Vector((0, 0, CHEST_Z))

    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)

    for obj in (body, chest, head, jaw, armL, armR, legL, legR, kneeL, kneeR, wingL, wingR, wtipL, wtipR):
        stash(obj, idle)
    for obj in (body, armL, armR, legL, legR, kneeL, kneeR, wingL, wingR, wtipL, wtipR):
        stash(obj, walk)
    for obj in (armL, jaw, head, wingL, wingR):
        stash(obj, punch)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "demon.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_demon] wrote", out_path)


if __name__ == "__main__":
    main()
