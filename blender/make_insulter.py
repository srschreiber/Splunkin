"""
Build the INSULTER enemy model -> assets/models/insulter.glb.

A stocky bald guy with a red goatee (an angry comedian type): bald skin head, red goatee on
the chin, bushy red eyebrows, a plain t-shirt + jeans. Same rig/clip conventions as the
skeleton (bones body/head/armL/armR/handL/handR + legL/legR; clips walk/punch; Z-up, +Y
front, feet ~Z-1.0). See docs/blender-model-scripting.md.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_insulter.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

SKIN  = (0.85, 0.66, 0.52, 1.0)   # bald head / arms
SHIRT = (0.55, 0.58, 0.62, 1.0)   # grey t-shirt
JEANS = (0.24, 0.30, 0.42, 1.0)   # blue jeans
RED   = (0.62, 0.26, 0.12, 1.0)   # red/ginger goatee + brows
DARK  = (0.05, 0.05, 0.06, 1.0)   # eyes
SHOE  = (0.10, 0.09, 0.08, 1.0)

def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass

def mat(name, rgba):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    if b: b.inputs["Base Color"].default_value = rgba
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
    skin, shirt, jeans = mat("skin", SKIN), mat("shirt", SHIRT), mat("jeans", JEANS)
    red, dark, shoe = mat("red", RED), mat("eye", DARK), mat("shoe", SHOE)

    body  = make_empty("body",  None, (0, 0, 0.0))
    head  = make_empty("head",  body, (0, 0, 0.92))
    armL  = make_empty("armL",  body, (0.30, 0, 0.80))
    armR  = make_empty("armR",  body, (-0.30, 0, 0.80))
    handL = make_empty("handL", armL, (0, 0, -0.52))
    handR = make_empty("handR", armR, (0, 0, -0.52))
    legL  = make_empty("legL",  body, (0.14, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.14, 0, 0.0))

    # Stocky torso (t-shirt) + a bit of a gut, jeans waist.
    make_part("shirt",  body, (0, 0, 0.50), (0.50, 0.34, 0.74), shirt)
    make_part("gut",    body, (0, 0.08, 0.30), (0.46, 0.30, 0.30), shirt)
    make_part("waist",  body, (0, 0, 0.06), (0.42, 0.28, 0.18), jeans)

    # Bald head (skin), angry eyes + bushy red brows, and a RED GOATEE on the chin (+Y front).
    make_part("skull", head, (0, 0, 0.20), (0.30, 0.30, 0.30), skin)
    make_part("eyeL",  head, (0.08, 0.15, 0.22), (0.05, 0.04, 0.05), dark)
    make_part("eyeR",  head, (-0.08, 0.15, 0.22), (0.05, 0.04, 0.05), dark)
    make_part("browL", head, (0.08, 0.15, 0.28), (0.10, 0.04, 0.03), red)   # bushy red brows
    make_part("browR", head, (-0.08, 0.15, 0.28), (0.10, 0.04, 0.03), red)
    make_part("mustache", head, (0, 0.16, 0.11), (0.14, 0.04, 0.03), red)   # connects to goatee
    make_part("goatee",   head, (0, 0.15, 0.03), (0.12, 0.06, 0.10), red)   # chin patch
    make_part("chinstrapL", head, (0.10, 0.10, 0.08), (0.03, 0.05, 0.10), red)
    make_part("chinstrapR", head, (-0.10, 0.10, 0.08), (0.03, 0.05, 0.10), red)

    # Arms (skin, short sleeves) + hands.
    make_part("sleeveL", armL, (0, 0, -0.12), (0.14, 0.14, 0.26), shirt)
    make_part("sleeveR", armR, (0, 0, -0.12), (0.14, 0.14, 0.26), shirt)
    make_part("foreL", armL, (0, 0, -0.40), (0.10, 0.10, 0.40), skin)
    make_part("foreR", armR, (0, 0, -0.40), (0.10, 0.10, 0.40), skin)
    make_part("handL", handL, (0, 0, -0.14), (0.11, 0.11, 0.16), skin)
    make_part("handR", handR, (0, 0, -0.14), (0.11, 0.11, 0.16), skin)

    # Legs (jeans) + shoes.
    make_part("legL", legL, (0, 0, -0.54), (0.16, 0.16, 1.04), jeans)
    make_part("legR", legR, (0, 0, -0.54), (0.16, 0.16, 1.04), jeans)
    make_part("shoeL", legL, (0, 0.06, -1.04), (0.17, 0.26, 0.12), shoe)
    make_part("shoeR", legR, (0, 0.06, -1.04), (0.17, 0.26, 0.12), shoe)

    # Animations: a swaggering walk + a dismissive arm-wave "punch" (he's gesturing, not fighting).
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 0), (8, 22), (20, -22), (32, 0)]:
        key_rot(legL, walk, frame, (a, 0, 0)); key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a*0.7, 0, 0)); key_rot(armR, walk, frame, (a*0.7, 0, 0))
    punch = bpy.data.actions.new("punch")
    for frame, a in [(1, 0), (6, -40), (12, 95), (18, 0)]:
        key_rot(armL, punch, frame, (a, 0, 0))

    for obj in (legL, legR, armL, armR):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk); stash(armR, walk); stash(armL, walk); stash(armL, punch)

    for obj in (body, head, armL, armR, handL, handR, legL, legR):
        obj.rotation_euler = (0, 0, 0)
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
