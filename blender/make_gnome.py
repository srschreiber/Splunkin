"""
Build a GNOME enemy (green clothes, pointy red hat, white beard, brown staff) and export
assets/models/gnome.glb. It re-skins the "flamethrower" enemy: same flame-cone behavior,
new look. See docs/blender-model-scripting.md.

Rig: Empties named body/head/armL/armR/handL/handR (+ legL/legR). Clips: walk, punch.
Z-up; front faces +Y; waist at origin; feet near Z=-1.0 so the model is grounded by the
engine's MODEL_FOOT_LIFT. Stout + short-legged so it reads as a gnome. The staff is a brown
box parented to the RIGHT hand and extending upward.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_gnome.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

GREEN = (0.13, 0.52, 0.20, 1.0)   # tunic / clothes
DGREEN= (0.09, 0.36, 0.14, 1.0)   # darker green (legs/boots)
RED   = (0.85, 0.12, 0.12, 1.0)   # pointy hat
SKIN  = (0.92, 0.74, 0.60, 1.0)   # face/hands
WHITE = (0.92, 0.92, 0.95, 1.0)   # beard
BROWN = (0.45, 0.29, 0.14, 1.0)   # staff
BLACK = (0.04, 0.04, 0.05, 1.0)   # eyes

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
    green, dgreen, red, skin, white, brown, black = (
        mat("g_tunic", GREEN), mat("g_legs", DGREEN), mat("g_hat", RED),
        mat("g_skin", SKIN), mat("g_beard", WHITE), mat("g_staff", BROWN), mat("g_eye", BLACK))

    # Stout, short-legged. Feet near Z=-1.0.
    body  = make_empty("body",  None, (0, 0, 0.0))
    head  = make_empty("head",  body, (0, 0, 0.30))
    armL  = make_empty("armL",  body, (0.30, 0, 0.12))
    armR  = make_empty("armR",  body, (-0.30, 0, 0.12))
    handL = make_empty("handL", armL, (0, 0, -0.34))
    handR = make_empty("handR", armR, (0, 0, -0.34))
    legL  = make_empty("legL",  body, (0.13, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.13, 0, 0.0))

    # Body: a wide green tunic; short dark-green legs/boots.
    make_part("tunic",  body, (0, 0, -0.30), (0.52, 0.42, 0.70), green)
    make_part("legbL",  legL, (0, 0, -0.80), (0.15, 0.15, 0.42), dgreen)   # -0.59..-1.01
    make_part("legbR",  legR, (0, 0, -0.80), (0.15, 0.15, 0.42), dgreen)

    # Head + face.
    make_part("face",   head, (0, 0.02, 0.06), (0.28, 0.26, 0.26), skin)
    make_part("eyeL",   head, (0.07, 0.15, 0.10), (0.04, 0.03, 0.04), black)
    make_part("eyeR",   head, (-0.07, 0.15, 0.10),(0.04, 0.03, 0.04), black)
    make_part("nose",   head, (0, 0.17, 0.02), (0.05, 0.06, 0.05), skin)
    # Big white beard hanging down over the tunic (a couple of boxes for fullness).
    make_part("beard1", head, (0, 0.12, -0.14), (0.26, 0.08, 0.26), white)
    make_part("beard2", head, (0, 0.10, -0.30), (0.18, 0.07, 0.16), white)
    make_part("mustache", head, (0, 0.16, -0.04), (0.20, 0.05, 0.05), white)

    # Pointy red hat: brim + tapering stacked boxes (cone) with a forward-drooping tip.
    make_part("brim",  head, (0, 0, 0.20), (0.36, 0.36, 0.06), red)
    make_part("hat1",  head, (0, 0, 0.28), (0.26, 0.26, 0.12), red)
    make_part("hat2",  head, (0, 0.01, 0.38), (0.17, 0.17, 0.12), red)
    make_part("hat3",  head, (0, 0.03, 0.47), (0.10, 0.10, 0.12), red)
    make_part("hattip",head, (0, 0.07, 0.55), (0.05, 0.05, 0.10), red)

    # Arms (skin hands), and the brown STAFF held in the right hand (extends up).
    make_part("uarmL", armL, (0, 0, -0.17), (0.10, 0.10, 0.34), green)
    make_part("handmL",handL,(0, 0, -0.06), (0.11, 0.11, 0.14), skin)
    make_part("uarmR", armR, (0, 0, -0.17), (0.10, 0.10, 0.34), green)
    make_part("handmR",handR,(0, 0, -0.06), (0.11, 0.11, 0.14), skin)
    make_part("staff", handR,(0, 0.06, 0.42), (0.05, 0.05, 1.05), brown)   # tall stick rising past the head
    make_part("staffknob", handR,(0, 0.06, 0.95), (0.09, 0.09, 0.09), brown)

    # Animations.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 22), (13, -22), (25, 22)]:
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a * 0.6, 0, 0))   # left arm swings; right holds the staff steadier
        key_rot(armR, walk, frame, (a * 0.3, 0, 0))

    punch = bpy.data.actions.new("punch")   # gnome "casts" with the left arm (forward swing)
    for frame, a in [(1, 0), (5, -35), (10, 95), (16, 0)]:
        key_rot(armL, punch, frame, (a, 0, 0))

    for obj in (legL, legR, armL, armR):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk); stash(armR, walk)
    stash(armL, walk); stash(armL, punch)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "gnome.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_gnome] wrote", out_path)

if __name__ == "__main__":
    main()
