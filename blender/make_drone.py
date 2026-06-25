"""
Build a gunner DRONE (small flying robot, 4 spinning propellers, glowing eye) and export
assets/models/drone.glb. See docs/blender-model-scripting.md.

Rig: Empties (4 rotor hubs) with blade boxes parented; a looping "walk" action spins the
rotors about Z (vertical). Z-up, front +Y, ORIGIN AT CENTER (drones hover — no foot lift).
Per-part materials show under a white tint.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_drone.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

BODY  = (0.26, 0.28, 0.34, 1.0)   # steel body
TRIM  = (0.30, 0.50, 0.92, 1.0)   # blue trim
LENS  = (0.95, 0.20, 0.15, 1.0)   # red eye (emissive)
ROTOR = (0.14, 0.14, 0.17, 1.0)   # rotor hub / arms
BLADE = (0.62, 0.64, 0.70, 1.0)   # propeller blades (light = motion blur read)

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
    e.empty_display_size = 0.05
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

def make_part(name, bone, center, size, material, rot=None):
    o = bpy.data.objects.new(name, box_mesh(name, size))
    bpy.context.collection.objects.link(o)
    o.parent = bone; o.matrix_parent_inverse = Matrix.Identity(4)
    o.location = Vector(center)
    if rot: o.rotation_euler = rot
    o.data.materials.append(material)
    return o

def main():
    reset_scene()
    body, trim, lens, rotor, blade = (
        mat("d_body", BODY), mat("d_trim", TRIM), mat("d_lens", LENS, emissive=True),
        mat("d_rotor", ROTOR), mat("d_blade", BLADE))

    root = bpy.data.objects.new("drone", None); bpy.context.collection.objects.link(root)

    # Body: rounded-ish hull + blue trim band + a red eye on the front (+Y) + skids.
    make_part("hull",  root, (0, 0, 0),       (0.22, 0.28, 0.13), body)
    make_part("band",  root, (0, 0, 0.04),    (0.24, 0.30, 0.04), trim)
    make_part("eye",   root, (0, 0.15, 0.02), (0.08, 0.04, 0.06), lens)
    make_part("skidL", root, (0.09, 0, -0.10),(0.03, 0.20, 0.06), rotor)
    make_part("skidR", root, (-0.09, 0, -0.10),(0.03, 0.20, 0.06), rotor)

    # Four arms + rotor hubs at the corners; blades parented to spinning hubs.
    walk = bpy.data.actions.new("walk")
    hubs = []
    corners = [(0.26, 0.26), (-0.26, 0.26), (0.26, -0.26), (-0.26, -0.26)]
    for i, (ax, ay) in enumerate(corners):
        make_part("arm%d" % i, root, (ax*0.5, ay*0.5, 0.02), (abs(ax)+0.04, 0.05, 0.04), rotor,
                  rot=(0, 0, math.atan2(ay, ax)))   # thin arm pointing outward
        hub = make_empty("rotor%d" % i, root, (ax, ay, 0.08))
        make_part("hubmesh%d" % i, hub, (0, 0, 0), (0.08, 0.08, 0.04), rotor)
        make_part("bladeA%d" % i, hub, (0, 0, 0.03), (0.34, 0.04, 0.012), blade)   # 2-blade prop
        make_part("bladeB%d" % i, hub, (0, 0, 0.03), (0.04, 0.34, 0.012), blade)
        hubs.append(hub)

    # walk = spin every rotor about Z (full turn over an 8-frame loop), phase-offset.
    for i, hub in enumerate(hubs):
        for f, deg in [(1, 0), (5, 180), (9, 360)]:
            if not hub.animation_data: hub.animation_data_create()
            hub.animation_data.action = walk
            hub.rotation_mode = 'XYZ'
            hub.rotation_euler = (0, 0, math.radians(deg + i * 30))
            hub.keyframe_insert(data_path="rotation_euler", frame=f)

    for hub in hubs:
        hub.animation_data.action = None
        tr = hub.animation_data.nla_tracks.new(); tr.name = "walk"
        tr.strips.new("walk", 1, walk)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "drone.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_drone] wrote", out_path)

if __name__ == "__main__":
    main()
