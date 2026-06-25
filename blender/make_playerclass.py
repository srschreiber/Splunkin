"""
Build the two PLAYER CLASS models with gear baked in, and export:
  assets/models/knight_class.glb  - steel knight, helm + sword baked in
  assets/models/wizard_class.glb   - blue-robe wizard, pointy hat + staff baked in

These REPLACE the generic player.glb for rendering a player of that class. Same rig/bone
names as the other humanoids (body/head/armL/armR/handL/handR + legL/legR) and the same
clip names the engine drives: walk, punch, block. The held weapon is parented to handL
(the punch clip is masked to armL, so it swings/casts), so no separate weapon prop is needed.
The shield is still drawn by the engine on the handR socket.

Conventions: Z-up, front +Y, feet near Z=-1.0, head ~Z+0.85 (matches player.glb bounds).
See docs/blender-model-scripting.md.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_playerclass.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

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
    e = bpy.data.objects.new(name, None); e.empty_display_size = 0.08
    bpy.context.collection.objects.link(e)
    if parent: e.parent = parent
    e.location = Vector(loc)
    return e

def box_mesh(name, size):
    sx, sy, sz = size[0]/2, size[1]/2, size[2]/2
    verts = [(-sx,-sy,-sz),(sx,-sy,-sz),(sx,sy,-sz),(-sx,sy,-sz),
             (-sx,-sy,sz),(sx,-sy,sz),(sx,sy,sz),(-sx,sy,sz)]
    faces = [(0,1,2,3),(7,6,5,4),(4,5,1,0),(6,7,3,2),(5,6,2,1),(7,4,0,3)]
    m = bpy.data.meshes.new(name); m.from_pydata(verts, [], faces); m.update()
    return m

def part(name, bone, center, size, material, rot=None):
    o = bpy.data.objects.new(name, box_mesh(name, size))
    bpy.context.collection.objects.link(o)
    o.parent = bone; o.matrix_parent_inverse = Matrix.Identity(4)
    o.location = Vector(center)
    if rot: o.rotation_euler = rot
    o.data.materials.append(material)
    return o

def key(obj, action, frame, euler_deg):
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.rotation_mode = 'XYZ'
    obj.rotation_euler = tuple(math.radians(a) for a in euler_deg)
    obj.keyframe_insert(data_path="rotation_euler", frame=frame)

def build(klass):
    reset_scene()
    wizard = (klass == "wizard")

    # Rig (human proportions; feet ~ -1.0). NOTE: armL is the bone the engine masks the
    # ATTACK clip to (so it holds the WEAPON) and armR gets the BLOCK clip (holds the SHIELD).
    # armL is placed on the character's LEFT (-X) so the weapon reads on the left arm.
    body  = make_empty("body",  None, (0, 0, 0.0))
    head  = make_empty("head",  body, (0, 0, 0.60))
    armL  = make_empty("armL",  body, (-0.26, 0, 0.52))   # LEFT side -> weapon
    armR  = make_empty("armR",  body, (0.26, 0, 0.52))    # RIGHT side -> shield
    handL = make_empty("handL", armL, (0, 0, -0.42))
    handR = make_empty("handR", armR, (0, 0, -0.42))
    legL  = make_empty("legL",  body, (-0.12, 0, 0.0))
    legR  = make_empty("legR",  body, (0.12, 0, 0.0))

    if wizard:
        robe  = mat("p_robe", (0.20, 0.26, 0.66, 1.0))
        drobe = mat("p_drobe", (0.13, 0.17, 0.48, 1.0))
        skin  = mat("p_skin", (0.90, 0.74, 0.62, 1.0))
        white = mat("p_white", (0.93, 0.93, 0.96, 1.0))
        hat   = mat("p_hat", (0.16, 0.20, 0.60, 1.0))
        star  = mat("p_star", (0.98, 0.85, 0.25, 1.0))
        wood  = mat("p_wood", (0.45, 0.29, 0.14, 1.0))
        orb   = mat("p_orb", (0.40, 0.78, 1.0, 1.0), emissive=True)
        # Robe body (flared hem hides legs) + arms.
        part("robeLo", body, (0, 0, -0.58), (0.54, 0.40, 0.84), robe)
        part("robeUp", body, (0, 0, 0.12), (0.42, 0.34, 0.62), robe)
        part("hem",    body, (0, 0, -0.98), (0.58, 0.44, 0.06), drobe)
        part("sash",   body, (0, 0.02, 0.0), (0.44, 0.36, 0.08), drobe)
        part("armmL",  armL, (0, 0, -0.21), (0.11, 0.11, 0.42), robe)
        part("armmR",  armR, (0, 0, -0.21), (0.11, 0.11, 0.42), robe)
        part("handmL", handL, (0, 0, -0.05), (0.11, 0.11, 0.12), skin)
        part("handmR", handR, (0, 0, -0.05), (0.11, 0.11, 0.12), skin)
        part("legbL",  legL, (0, 0, -0.85), (0.12, 0.12, 0.34), drobe)
        part("legbR",  legR, (0, 0, -0.85), (0.12, 0.12, 0.34), drobe)
        # Head + beard.
        part("face",   head, (0, 0.02, 0.06), (0.26, 0.24, 0.26), skin)
        part("beard",  head, (0, 0.11, -0.16), (0.22, 0.08, 0.28), white)
        # Pointy wizard hat (baked).
        part("brim",  head, (0, 0, 0.22), (0.46, 0.46, 0.05), hat)
        part("hat1",  head, (0, 0, 0.30), (0.27, 0.27, 0.14), hat)
        part("hat2",  head, (0, 0.02, 0.43), (0.18, 0.18, 0.16), hat)
        part("hat3",  head, (0, 0.05, 0.57), (0.11, 0.11, 0.16), hat)
        part("hat4",  head, (0, 0.09, 0.68), (0.06, 0.06, 0.13), hat)
        part("hstar", head, (0, 0.16, 0.36), (0.07, 0.02, 0.07), star)
        # Staff baked into the LEFT hand (extends forward/down from the grip).
        part("staff", handL, (0, 0.02, -0.55), (0.05, 0.05, 1.1), wood)
        part("orb",   handL, (0, 0.02, -1.08), (0.16, 0.16, 0.16), orb)
        # (No baked shield — the wizard's shield is a translucent magic barrier drawn in-engine.)
    else:
        steel = mat("p_steel", (0.55, 0.58, 0.65, 1.0))
        dark  = mat("p_dark", (0.34, 0.36, 0.42, 1.0))
        skin  = mat("p_skin", (0.90, 0.74, 0.62, 1.0))
        plume = mat("p_plume", (0.85, 0.15, 0.15, 1.0))
        blade = mat("p_blade", (0.80, 0.82, 0.90, 1.0))
        grip  = mat("p_grip", (0.30, 0.20, 0.12, 1.0))
        # Plated body.
        part("torso",  body, (0, 0, 0.30), (0.50, 0.34, 0.66), steel)
        part("belt",   body, (0, 0, -0.04), (0.46, 0.32, 0.12), dark)
        part("skirt",  body, (0, 0, -0.30), (0.44, 0.30, 0.34), dark)
        part("pauldL", body, (-0.30, 0, 0.52), (0.26, 0.26, 0.20), steel)
        part("pauldR", body, (0.30, 0, 0.52), (0.26, 0.26, 0.20), steel)
        part("armmL",  armL, (0, 0, -0.21), (0.12, 0.12, 0.42), steel)
        part("armmR",  armR, (0, 0, -0.21), (0.12, 0.12, 0.42), steel)
        part("handmL", handL, (0, 0, -0.06), (0.12, 0.12, 0.14), dark)
        part("handmR", handR, (0, 0, -0.06), (0.12, 0.12, 0.14), dark)
        part("legbL",  legL, (0, 0, -0.55), (0.16, 0.16, 1.0), steel)
        part("legbR",  legR, (0, 0, -0.55), (0.16, 0.16, 1.0), steel)
        part("bootL",  legL, (0, 0.05, -1.0), (0.18, 0.26, 0.16), dark)
        part("bootR",  legR, (0, 0.05, -1.0), (0.18, 0.26, 0.16), dark)
        # Helm (baked) with visor slit + red plume.
        part("helm",  head, (0, 0, 0.10), (0.36, 0.36, 0.36), steel)
        part("visor", head, (0, 0.17, 0.08), (0.38, 0.08, 0.16), dark)
        part("nasal", head, (0, 0.19, 0.0), (0.05, 0.10, 0.18), steel)
        part("crest", head, (0, 0, 0.30), (0.05, 0.30, 0.10), dark)
        part("plume", head, (0, -0.10, 0.36), (0.08, 0.12, 0.26), plume)
        # Sword baked into the LEFT hand (blade points forward/down).
        part("grip",  handL, (0, 0, -0.18), (0.05, 0.05, 0.22), grip)
        part("guard", handL, (0, 0, -0.30), (0.30, 0.07, 0.06), steel)
        part("blade", handL, (0, 0, -0.78), (0.09, 0.03, 0.95), blade)
        # Kite shield baked on the RIGHT hand (raises on block), face toward +Y, tapering down.
        part("shieldU", handR, (0, 0.14, -0.06), (0.42, 0.05, 0.34), steel)
        part("shieldL", handR, (0, 0.14, -0.34), (0.30, 0.05, 0.30), steel, rot=(math.radians(0), 0, math.radians(45)))
        part("shieldBoss", handR, (0, 0.19, -0.10), (0.10, 0.05, 0.10), dark)

    # --- Animations (walk / punch / block). ---
    walk = bpy.data.actions.new("walk")
    for f, a in [(1, 22), (13, -22), (25, 22)]:
        key(legL, walk, f, (a, 0, 0)); key(legR, walk, f, (-a, 0, 0))
        key(armL, walk, f, (-a*0.6, 0, 0)); key(armR, walk, f, (a*0.6, 0, 0))

    punch = bpy.data.actions.new("punch")    # left arm swings/casts FORWARD (out the front,
    # matching the enemy models: small wind-up back, then a big +X swing into the scene).
    for f, a in [(1, 0), (4, -35), (9, 95), (16, 0)]:
        key(armL, punch, f, (a, 0, 0))

    block = bpy.data.actions.new("block")    # right arm raises the shield across the body
    for f, a in [(1, 0), (8, -95)]:
        key(armR, block, f, (a, 0, 20))

    # Stash each clip on the bones it animates so the exporter writes all three by name.
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        obj.animation_data.action = None
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    for obj in (legL, legR, armL, armR):
        if obj.animation_data: obj.animation_data.action = None
    stash(legL, walk); stash(legR, walk); stash(armR, walk); stash(armL, walk)
    stash(armL, punch); stash(armR, block)

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
