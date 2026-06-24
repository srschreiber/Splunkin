"""
Build a MAGE enemy (blue robe, tall pointy blue wizard hat, white beard, staff with a
glowing orb) and export assets/models/mage.glb. Re-skins the "ranged" enemy: same shoot
behavior, new look. See docs/blender-model-scripting.md.

Rig: Empties body/head/armL/armR/handL/handR (+ legL/legR). Clips: walk, punch. Z-up,
front +Y, waist at origin, feet near Z=-1.0. Staff (brown) + emissive orb in the right hand.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_mage.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

ROBE  = (0.20, 0.26, 0.68, 1.0)   # blue robe
DROBE = (0.13, 0.17, 0.48, 1.0)   # darker blue trim
HAT   = (0.16, 0.20, 0.60, 1.0)   # wizard hat
STAR  = (0.98, 0.85, 0.25, 1.0)   # gold star on the hat
SKIN  = (0.90, 0.74, 0.62, 1.0)
WHITE = (0.93, 0.93, 0.96, 1.0)   # beard
BROWN = (0.45, 0.29, 0.14, 1.0)   # staff
ORB   = (0.35, 0.75, 1.00, 1.0)   # glowing staff orb
BLACK = (0.04, 0.04, 0.05, 1.0)

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

def box_mesh(name, size):
    sx, sy, sz = size[0]/2, size[1]/2, size[2]/2
    verts = [(-sx,-sy,-sz),(sx,-sy,-sz),(sx,sy,-sz),(-sx,sy,-sz),
             (-sx,-sy,sz),(sx,-sy,sz),(sx,sy,sz),(-sx,sy,sz)]
    faces = [(0,1,2,3),(7,6,5,4),(4,5,1,0),(6,7,3,2),(5,6,2,1),(7,4,0,3)]
    m = bpy.data.meshes.new(name); m.from_pydata(verts, [], faces); m.update()
    return m

def make_part(name, bone, center, size, material):
    o = bpy.data.objects.new(name, box_mesh(name, size))
    bpy.context.collection.objects.link(o)
    o.parent = bone; o.matrix_parent_inverse = Matrix.Identity(4)
    o.location = Vector(center)
    o.data.materials.append(material)
    return o

def key_rot(obj, action, frame, euler_deg):
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.rotation_mode = 'XYZ'
    obj.rotation_euler = tuple(math.radians(a) for a in euler_deg)
    obj.keyframe_insert(data_path="rotation_euler", frame=frame)

def main():
    reset_scene()
    robe, drobe, hat, star, skin, white, brown, orb, black = (
        mat("m_robe", ROBE), mat("m_trim", DROBE), mat("m_hat", HAT), mat("m_star", STAR),
        mat("m_skin", SKIN), mat("m_beard", WHITE), mat("m_staff", BROWN),
        mat("m_orb", ORB, emissive=True), mat("m_eye", BLACK))

    body  = make_empty("body",  None, (0, 0, 0.0))
    head  = make_empty("head",  body, (0, 0, 0.50))
    armL  = make_empty("armL",  body, (0.26, 0, 0.34))
    armR  = make_empty("armR",  body, (-0.26, 0, 0.34))
    handL = make_empty("handL", armL, (0, 0, -0.40))
    handR = make_empty("handR", armR, (0, 0, -0.40))
    legL  = make_empty("legL",  body, (0.11, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.11, 0, 0.0))

    # Robe: a tall tapering body (wider at the hem) reaching the floor; hides the legs.
    make_part("robeLo", body, (0, 0, -0.62), (0.56, 0.40, 0.80), robe)   # flared hem -1.02..-0.22
    make_part("robeUp", body, (0, 0, 0.05), (0.40, 0.34, 0.62), robe)    # torso
    make_part("hem",    body, (0, 0, -1.00), (0.60, 0.44, 0.06), drobe)  # bottom trim
    make_part("sash",   body, (0, 0.02, -0.05), (0.42, 0.36, 0.08), drobe)
    make_part("legbL",  legL, (0, 0, -0.85), (0.12, 0.12, 0.32), drobe)  # just a hint of feet
    make_part("legbR",  legR, (0, 0, -0.85), (0.12, 0.12, 0.32), drobe)

    # Head + long white beard.
    make_part("face",   head, (0, 0.02, 0.05), (0.26, 0.24, 0.24), skin)
    make_part("eyeL",   head, (0.06, 0.14, 0.09), (0.04, 0.03, 0.04), black)
    make_part("eyeR",   head, (-0.06, 0.14, 0.09),(0.04, 0.03, 0.04), black)
    make_part("beard1", head, (0, 0.11, -0.16), (0.22, 0.08, 0.30), white)
    make_part("beard2", head, (0, 0.08, -0.36), (0.14, 0.07, 0.22), white)

    # Tall pointy blue wizard hat (taller than the gnome's): brim + tapering stack + star.
    make_part("brim",  head, (0, 0, 0.18), (0.40, 0.40, 0.05), hat)
    make_part("hat1",  head, (0, 0.00, 0.27), (0.26, 0.26, 0.14), hat)
    make_part("hat2",  head, (0, 0.02, 0.40), (0.18, 0.18, 0.14), hat)
    make_part("hat3",  head, (0, 0.05, 0.53), (0.11, 0.11, 0.14), hat)
    make_part("hat4",  head, (0, 0.09, 0.64), (0.06, 0.06, 0.12), hat)
    make_part("star",  head, (0, 0.16, 0.34), (0.07, 0.02, 0.07), star)   # little gold star on the front

    # Arms + staff with a glowing orb (held in the right hand).
    make_part("armmL", armL, (0, 0, -0.20), (0.10, 0.10, 0.40), robe)
    make_part("handmL",handL,(0, 0, -0.05), (0.10, 0.10, 0.12), skin)
    make_part("armmR", armR, (0, 0, -0.20), (0.10, 0.10, 0.40), robe)
    make_part("handmR",handR,(0, 0, -0.05), (0.10, 0.10, 0.12), skin)
    make_part("staff", handR,(0, 0.05, 0.45), (0.045, 0.045, 1.10), brown)
    make_part("orb",   handR,(0, 0.05, 1.02), (0.13, 0.13, 0.13), orb)     # glowing tip

    # Animations.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 16), (13, -16), (25, 16)]:   # gentle glide; robe hides big leg motion
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a, 0, 0))

    punch = bpy.data.actions.new("punch")   # raise the staff arm to "cast"
    for frame, a in [(1, 0), (6, -55), (12, 20), (18, 0)]:
        key_rot(armL, punch, frame, (a, 0, 0))

    for obj in (legL, legR, armL):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk)
    stash(armL, walk); stash(armL, punch)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "mage.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_mage] wrote", out_path)

if __name__ == "__main__":
    main()
