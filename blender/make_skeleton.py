"""
Procedurally build a low-poly SKELETON enemy and export assets/models/skeleton.glb.

IMPORTANT: this engine does NOT do GPU skinning. It animates by rigidly attaching each
mesh part to a BONE NODE (the part follows its node's animated world transform). So this
script builds the rig as a hierarchy of EMPTIES (named body/head/armL/armR/handL/handR
plus legL/legR) and parents separate mesh boxes to each. A single skinned mesh would load
as one static part and never animate. See docs/blender-model-scripting.md.

Detail note: the engine's model shader is flat per-part color (no texture sampling), so
"detail" = more geometry + per-part material colors (ribs, eye sockets, jaw), not painted
textures.

Conventions: bones named body/head/armL/armR/handL/handR; clips named walk/punch; Z-up;
waist at origin, feet near Z=-1.15, head up high so the model reads tall (~matches player).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_skeleton.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

BONE  = (0.88, 0.87, 0.80, 1.0)   # bone white
DARK  = (0.05, 0.05, 0.06, 1.0)   # eye sockets / hollows
EMBER = (0.95, 0.35, 0.08, 1.0)   # glowing eye dots

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
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = 3.0
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
    bone, dark, ember = mat("bone", BONE), mat("hollow", DARK), mat("ember", EMBER, emissive=True)

    # --- Rig (empties). Taller than before: head bone higher, longer legs/spine. ---
    # FRONT of the skeleton faces +Y (so eye sockets go on the +Y face of the skull).
    body  = make_empty("body",  None, (0, 0, 0.0))
    head  = make_empty("head",  body, (0, 0, 0.92))
    armL  = make_empty("armL",  body, (0.26, 0, 0.82))
    armR  = make_empty("armR",  body, (-0.26, 0, 0.82))
    handL = make_empty("handL", armL, (0, 0, -0.50))
    handR = make_empty("handR", armR, (0, 0, -0.50))
    legL  = make_empty("legL",  body, (0.12, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.12, 0, 0.0))

    # --- Skull: cranium + jaw + recessed eye sockets with ember dots + nose hole ---
    make_part("cranium", head, (0, 0, 0.20),  (0.26, 0.26, 0.26), bone)
    make_part("jaw",     head, (0, 0.03, 0.05),(0.22, 0.20, 0.08), bone)
    make_part("socketL", head, (0.07, 0.13, 0.20), (0.09, 0.06, 0.09), dark)   # +Y = front
    make_part("socketR", head, (-0.07, 0.13, 0.20),(0.09, 0.06, 0.09), dark)
    make_part("eyeL",    head, (0.07, 0.15, 0.20), (0.04, 0.04, 0.04), ember)
    make_part("eyeR",    head, (-0.07, 0.15, 0.20),(0.04, 0.04, 0.04), ember)
    make_part("nose",    head, (0, 0.14, 0.12), (0.04, 0.05, 0.06), dark)

    # --- Ribcage: spine + sternum + paired curved-ish ribs (separate boxes) ---
    make_part("spine",  body, (0, -0.04, 0.52), (0.06, 0.06, 0.78), bone)   # behind, vertical
    make_part("sternum",body, (0, 0.06, 0.54),  (0.06, 0.05, 0.40), bone)   # front center
    for i, z in enumerate((0.38, 0.52, 0.66)):
        w = 0.30 - i * 0.02
        make_part("ribU%d" % i, body, (0,  0.02, z), (w, 0.13, 0.035), bone)   # each rib = a thin flat bar
    make_part("collar", body, (0, 0.03, 0.76), (0.34, 0.08, 0.05), bone)       # collarbone
    make_part("pelvis", body, (0, 0, 0.06), (0.30, 0.16, 0.18), bone)

    # --- Limbs (thin bones). Legs are kept so the feet land near Z=-1.05 (grounded). ---
    make_part("uarmL", armL, (0, 0, -0.25), (0.06, 0.06, 0.52), bone)
    make_part("farmL", handL,(0, 0, -0.13), (0.07, 0.07, 0.28), bone)
    make_part("uarmR", armR, (0, 0, -0.25), (0.06, 0.06, 0.52), bone)
    make_part("farmR", handR,(0, 0, -0.13), (0.07, 0.07, 0.28), bone)
    make_part("legbL", legL, (0, 0, -0.54), (0.08, 0.08, 1.08), bone)
    make_part("legbR", legR, (0, 0, -0.54), (0.08, 0.08, 1.08), bone)

    # --- Animations ---
    # walk: legs + arms swing opposite, 24-frame loop.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 25), (13, -25), (25, 25)]:
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a * 0.7, 0, 0))
        key_rot(armR, walk, frame, (a * 0.7, 0, 0))

    # punch: armL winds back (-X swings it backward) then swings FORWARD/down.
    # The arm hangs along -Z; a +X rotation sweeps -Z toward +Y (forward), so the big
    # extension angle is POSITIVE (the previous negative value swung it backwards).
    punch = bpy.data.actions.new("punch")
    for frame, a in [(1, 0), (5, -40), (10, 100), (16, 0)]:
        key_rot(armL, punch, frame, (a, 0, 0))
    key_rot(handL, punch, 10, (25, 0, 0))
    key_rot(handL, punch, 16, (0, 0, 0))

    for obj in (legL, legR, armL, armR, handL):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk); stash(armR, walk)
    stash(armL, walk); stash(armL, punch); stash(handL, punch)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "skeleton.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_skeleton] wrote", out_path)

if __name__ == "__main__":
    main()
