"""
Build a floating EYE enemy (round eyeball, red veins, animated red tentacles trailing
behind) and export assets/models/eye.glb. Replaces the old eye_enemy.glb look.

Notes vs the character models:
  * Uses Blender sphere/cylinder PRIMITIVES (not just boxes) for a proper round eye.
  * No skinning: tentacles are chains of EMPTIES with a tapering cylinder parented to each
    segment; a looping "walk" action undulates them (the game plays it continuously).
  * Per-part material colors (white sclera, red iris, black pupil, red veins/tentacles)
    show because the engine draws the model with a white tint.
  * Front faces +Y (the iris/pupil), tentacles trail -Y. The game yaws the eye toward its
    target and pitches it to look up/down.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_eye.py
"""

import bpy, math, os
from mathutils import Vector

WHITE = (0.90, 0.90, 0.92, 1.0)   # sclera
IRIS  = (0.80, 0.12, 0.10, 1.0)   # angry red iris
PUPIL = (0.03, 0.03, 0.04, 1.0)
VEIN  = (0.70, 0.07, 0.07, 1.0)
TENT  = (0.62, 0.06, 0.06, 1.0)

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
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = 2.5
    return m

def make_empty(name, parent, loc, rot=(0, 0, 0)):
    e = bpy.data.objects.new(name, None)
    e.empty_display_size = 0.06
    bpy.context.collection.objects.link(e)
    if parent: e.parent = parent
    e.location = Vector(loc); e.rotation_euler = rot
    return e

def sphere(name, parent, loc, radius, scale, material):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=14, ring_count=8, radius=radius, location=(0, 0, 0))
    o = bpy.context.active_object; o.name = name
    if parent: o.parent = parent
    o.location = Vector(loc); o.scale = Vector(scale)
    o.data.materials.append(material)
    bpy.ops.object.shade_smooth()
    return o

def cyl(name, parent, loc, radius, depth, material, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cylinder_add(vertices=8, radius=radius, depth=depth, location=(0, 0, 0))
    o = bpy.context.active_object; o.name = name
    if parent: o.parent = parent
    o.location = Vector(loc); o.rotation_euler = rot
    o.data.materials.append(material)
    return o

def key_rot_x(obj, action, frame, deg):
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.rotation_mode = 'XYZ'
    obj.rotation_euler = (obj.rotation_euler[0], obj.rotation_euler[1], math.radians(deg))
    # we wave about the LOCAL X by writing rotation_euler[0]; keep current y/z
    obj.rotation_euler = (math.radians(deg), obj.rotation_euler[1], obj.rotation_euler[2])
    obj.keyframe_insert(data_path="rotation_euler", frame=frame)

def main():
    reset_scene()
    white = mat("e_sclera", WHITE)
    iris  = mat("e_iris", IRIS, emissive=True)
    pupil = mat("e_pupil", PUPIL)
    vein  = mat("e_vein", VEIN)
    tent  = mat("e_tent", TENT)

    root = bpy.data.objects.new("eye", None); bpy.context.collection.objects.link(root)

    # Eyeball: white sclera + red iris disc + black pupil on the +Y face.
    sphere("sclera", root, (0, 0, 0), 0.45, (1, 1, 1), white)
    sphere("iris",   root, (0, 0.40, 0), 0.22, (1, 0.4, 1), iris)     # flattened against the front
    sphere("pupil",  root, (0, 0.47, 0), 0.10, (1, 0.4, 1), pupil)

    # Red veins: thin cylinders lying front-to-back over the sclera surface.
    import math as _m
    for i, ang in enumerate([20, 70, 150, 210, 290, 340]):
        a = _m.radians(ang)
        x, z = _m.cos(a) * 0.42, _m.sin(a) * 0.42
        # cylinder runs along Y (front-back): rotate 90 about X so its length is on Y.
        cyl("vein%d" % i, root, (x, -0.02, z), 0.012, 0.62, vein, rot=(_m.radians(90), 0, 0))

    # Tentacles: chains of empties trailing -Y and drooping, each with a tapering cylinder.
    # A "walk" action undulates them (phase offset per tentacle and per segment).
    walk = bpy.data.actions.new("walk")
    seg_objs = []
    NT, NS = 6, 3
    for t in range(NT):
        spread = _m.radians(-50 + t * (100.0 / (NT - 1)))          # fan across the back
        bx, bz = _m.sin(spread) * 0.34, _m.cos(spread) * 0.08 - 0.18
        # Root empty: tilt so its local -Z points backward (-Y) and a bit down.
        seg = make_empty("tent%d_0" % t, root, (bx, -0.34, bz), rot=(_m.radians(-72), 0, spread * 0.4))
        chain = [seg]
        for s in range(1, NS):
            seg = make_empty("tent%d_%d" % (t, s), chain[-1], (0, 0, -0.26))
            chain.append(seg)
        for s, e in enumerate(chain):
            taper = 0.055 - s * 0.013
            cyl("tentmesh%d_%d" % (t, s), e, (0, 0, -0.13), taper, 0.26, tent)
            seg_objs.append((e, t, s))

    # Keyframe a travelling sine wave on each segment's local-X rotation.
    frames = [1, 7, 13, 19, 25]
    for (e, t, s) in seg_objs:
        for f in frames:
            phase = (f / 24.0) * 2 * _m.pi + t * 0.7 + s * 0.9
            amp = 10 + s * 6                     # tips swing more
            key_rot_x(e, walk, f, amp * _m.sin(phase))

    # Stash "walk" on every segment so the exporter merges them into one named animation.
    for (e, t, s) in seg_objs:
        e.animation_data.action = None
        tr = e.animation_data.nla_tracks.new(); tr.name = "walk"
        tr.strips.new("walk", 1, walk)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "eye.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_eye] wrote", out_path)

if __name__ == "__main__":
    main()
