"""
Build the SCAVENGER friendly-mob model -> assets/models/scavenger.glb.

A dapper gentleman who roams the battlefield collecting dropped gold: dark suit, white
shirt, TOP HAT, MONOCLE, mustache, and a CANE held in the right hand. Same rig/clip
conventions as the skeleton (bones body/head/armL/armR/handL/handR + legL/legR; clips
walk/punch; Z-up, +Y front, feet ~Z-1.0). See docs/blender-model-scripting.md.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_scavenger.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

SUIT   = (0.10, 0.11, 0.16, 1.0)   # charcoal-navy jacket/trousers
SHIRT  = (0.92, 0.92, 0.95, 1.0)   # white shirt front
SKIN   = (0.86, 0.70, 0.56, 1.0)   # face/hands
HAT    = (0.04, 0.04, 0.05, 1.0)   # black top hat
GOLD   = (0.95, 0.78, 0.22, 1.0)   # monocle rim + cane knob + hat band
DARK   = (0.03, 0.03, 0.04, 1.0)   # eyes / monocle lens
STACHE = (0.18, 0.10, 0.05, 1.0)   # brown mustache
WOOD   = (0.20, 0.12, 0.07, 1.0)   # cane shaft

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
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = 2.0
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
    suit, shirt, skin = mat("suit", SUIT), mat("shirt", SHIRT), mat("skin", SKIN)
    hat, gold, dark = mat("hat", HAT), mat("gold", GOLD, emissive=True), mat("eye", DARK)
    stache, wood = mat("stache", STACHE), mat("wood", WOOD)

    # Rig (faces +Y). armL on +X, armR on -X (cane goes in the right hand).
    body  = make_empty("body",  None, (0, 0, 0.0))
    head  = make_empty("head",  body, (0, 0, 0.92))
    armL  = make_empty("armL",  body, (0.26, 0, 0.82))
    armR  = make_empty("armR",  body, (-0.26, 0, 0.82))
    handL = make_empty("handL", armL, (0, 0, -0.50))
    handR = make_empty("handR", armR, (0, 0, -0.50))
    legL  = make_empty("legL",  body, (0.12, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.12, 0, 0.0))

    # Torso: suit jacket with a white shirt strip + lapels, pelvis/trousers top.
    make_part("jacket",  body, (0, 0, 0.52), (0.40, 0.26, 0.78), suit)
    make_part("shirt",   body, (0, 0.10, 0.54), (0.10, 0.08, 0.62), shirt)
    make_part("lapelL",  body, (0.07, 0.12, 0.66), (0.05, 0.04, 0.22), suit)
    make_part("lapelR",  body, (-0.07, 0.12, 0.66), (0.05, 0.04, 0.22), suit)
    make_part("bowtie",  body, (0, 0.13, 0.74), (0.10, 0.04, 0.04), dark)
    make_part("pelvis",  body, (0, 0, 0.08), (0.34, 0.20, 0.20), suit)

    # Head: skin face + eyes + mustache.
    make_part("face",  head, (0, 0, 0.18), (0.26, 0.24, 0.26), skin)
    make_part("eyeL",  head, (0.07, 0.13, 0.22), (0.04, 0.03, 0.04), dark)
    make_part("eyeR",  head, (-0.07, 0.13, 0.22), (0.04, 0.03, 0.04), dark)
    make_part("stache",head, (0, 0.14, 0.10), (0.18, 0.04, 0.04), stache)
    # Monocle around the RIGHT eye (-X): gold rim + dark lens, on the +Y face.
    make_part("mono_rim",  head, (-0.07, 0.135, 0.22), (0.11, 0.02, 0.11), gold)
    make_part("mono_lens", head, (-0.07, 0.15, 0.22), (0.07, 0.01, 0.07), dark)

    # TOP HAT: brim + tall body + gold band, stacked on the crown.
    make_part("hat_brim", head, (0, 0, 0.34), (0.40, 0.40, 0.04), hat)
    make_part("hat_body", head, (0, 0, 0.52), (0.26, 0.26, 0.34), hat)
    make_part("hat_band", head, (0, 0, 0.38), (0.27, 0.27, 0.05), gold)

    # Arms: suit sleeves + skin hands.
    make_part("sleeveL", armL, (0, 0, -0.25), (0.10, 0.10, 0.52), suit)
    make_part("sleeveR", armR, (0, 0, -0.25), (0.10, 0.10, 0.52), suit)
    make_part("handL",   handL, (0, 0, -0.13), (0.09, 0.09, 0.18), skin)
    make_part("handR",   handR, (0, 0, -0.13), (0.09, 0.09, 0.18), skin)

    # Legs: suit trousers + shoes.
    make_part("legL", legL, (0, 0, -0.54), (0.12, 0.12, 1.04), suit)
    make_part("legR", legR, (0, 0, -0.54), (0.12, 0.12, 1.04), suit)
    make_part("shoeL", legL, (0, 0.06, -1.04), (0.13, 0.24, 0.12), dark)
    make_part("shoeR", legR, (0, 0.06, -1.04), (0.13, 0.24, 0.12), dark)

    # CANE held in the right hand: wood shaft to the ground + a gold knob on top.
    make_part("cane_shaft", handR, (0, 0.16, -0.62), (0.05, 0.05, 1.5), wood)
    make_part("cane_knob",  handR, (0, 0.16, 0.16), (0.09, 0.09, 0.12), gold)

    # --- Animations ---
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 0), (8, 20), (20, -20), (32, 0)]:   # start neutral so rest pose is upright
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a * 0.6, 0, 0))         # left arm swings; right arm holds the cane
    punch = bpy.data.actions.new("punch")                    # a little cane jab (self-defense)
    for frame, a in [(1, 0), (5, -30), (10, 80), (16, 0)]:
        key_rot(armR, punch, frame, (a, 0, 0))

    for obj in (legL, legR, armL, armR):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk); stash(armL, walk); stash(armR, punch)

    # Reset every bone to neutral + park on frame 1 so the exported REST pose is upright.
    for obj in (body, head, armL, armR, handL, handR, legL, legR):
        obj.rotation_euler = (0, 0, 0)
    bpy.context.scene.frame_set(1)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "scavenger.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_scavenger] wrote", out_path)

if __name__ == "__main__":
    main()
