"""
Build a defensive SOLAR TURRET (mount + rotating mantle with a dish/barrel) and export
assets/models/turret.glb. See docs/blender-model-scripting.md.

REVAMP: a smooth domed base on splayed legs, a rounded mantle with beveled side panels, a
tapered barrel and a shallow solar dish, and a glowing targeting lens. Detail is added cheaply
by JOINing same-material geometry on a bone into one primitive (one draw call), like
make_skeleton.py.

The game draws this housing yawed toward the current target and still draws an *aimed* barrel
procedurally on top, so the model only needs the parts that don't move per-frame. It is mostly
static: pose_model is called with no layers (rest pose). We ADD an "idle" clip (slow scanning
yaw of the mantle + a subtle bob) so a turret between targets reads as alive. There are no
engine-driven aim bones here (the engine aims a separate procedural barrel), so the scanning
mantle bone does not fight anything.

Rig (Empties): turret(root) -> mantle.  Base/legs ride the static root; the mantle (housing +
barrel + dish) scans in idle.
Conventions: Z-up; ORIGIN AT GROUND (Z=0 on the floor); front faces +Y (barrel exits front).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_turret.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

GUN   = (0.20, 0.21, 0.25, 1.0)   # dark gunmetal
STEEL = (0.46, 0.48, 0.54, 1.0)   # lighter steel
ACCENT= (0.85, 0.45, 0.15, 1.0)   # orange trim / panels
GLOW  = (0.98, 0.55, 0.18, 1.0)   # emissive lens / dish core


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
    e.empty_display_size = 0.1
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

def bone_cyl(name, r_bot, r_top, depth, loc, mat_, rot=None, segs=14):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs,
                          radius1=r_bot, radius2=r_top, depth=depth)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), True)

def box(name, size, loc, mat_, rot=None, bev=0.02):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), False)


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
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.location = Vector(loc)
    obj.keyframe_insert(data_path="location", frame=frame)


MZ = 0.66   # mantle pivot height


def main():
    reset_scene()
    gun, steel, accent, glow = (mat("t_gun", GUN), mat("t_steel", STEEL),
                                mat("t_accent", ACCENT), mat("t_glow", GLOW, emissive=True))

    # --- Rig (empties). FRONT faces +Y. ---
    root   = make_empty("turret", None, (0, 0, 0))
    mantle = make_empty("mantle", root, (0, 0, MZ))

    # --- MOUNT (static, on root): splayed tripod legs with feet, a smooth domed base, and a
    # swivel collar the mantle rests on. ---
    mount = []
    for i in range(3):
        ang = math.radians(90 + i * 120)
        dx, dy = math.cos(ang) * 0.40, math.sin(ang) * 0.40
        # tapered leg leaning outward, foot pad at the base
        mount.append(bone_cyl("leg%d" % i, 0.10, 0.055, 0.50, (dx, dy, 0.26), steel,
                              rot=(math.degrees(-0.32) * math.sin(ang),
                                   math.degrees(0.32) * math.cos(ang), 0), segs=10))
        mount.append(bone_cyl("foot%d" % i, 0.13, 0.10, 0.06, (dx * 1.3, dy * 1.3, 0.03), gun, segs=12))
    mount.append(sphere("dome",  0.40, (0, 0, 0.40), gun, scale=(1.0, 1.0, 0.62), subdiv=3))  # domed base
    mount.append(bone_cyl("collar", 0.30, 0.26, 0.12, (0, 0, 0.56), steel, segs=20))           # swivel collar
    mount.append(bone_cyl("ringtrim", 0.31, 0.31, 0.03, (0, 0, 0.52), accent, segs=20))        # accent ring
    assemble("mount", root, mount)

    # --- MANTLE (scans in idle): rounded housing, beveled side cheeks, a backpack
    # counterweight, a tapered barrel + muzzle, a shallow solar dish, and a glowing lens. ---
    head = [
        sphere("housing", 0.30, (0, -0.02, 0.02), gun, scale=(1.0, 1.05, 0.85), subdiv=3),
        box("cheekL", (0.10, 0.40, 0.34), (0.27, -0.02, 0.0), steel, rot=(0, 0, 6), bev=0.03),
        box("cheekR", (0.10, 0.40, 0.34), (-0.27, -0.02, 0.0), steel, rot=(0, 0, -6), bev=0.03),
        box("brow",   (0.46, 0.16, 0.16), (0, 0.26, 0.14), steel, bev=0.03),
        sphere("backpack", 0.20, (0, -0.30, 0.04), steel, scale=(1.1, 0.9, 1.0), subdiv=2),
        # barrel exits the front (+Y): tapered cannon + muzzle ring
        bone_cyl("barrel", 0.075, 0.06, 0.52, (0, 0.46, 0.03), gun, rot=(-90, 0, 0), segs=14),
        bone_cyl("muzzle", 0.085, 0.085, 0.06, (0, 0.70, 0.03), steel, rot=(-90, 0, 0), segs=14),
        # shallow solar dish ringing the barrel base, concave toward +Y
        bone_cyl("dish", 0.10, 0.26, 0.12, (0, 0.30, 0.04), steel, rot=(-90, 0, 0), segs=20),
        # glowing targeting lens + dish core
        sphere("lens", 0.06, (0, 0.32, 0.04), glow, scale=(1.0, 0.5, 1.0), subdiv=2),
        sphere("corelite", 0.05, (0, 0.205, 0.04), glow, subdiv=2),
        box("ventL", (0.05, 0.22, 0.20), (0.295, -0.16, 0.0), accent, bev=0.015),
        box("ventR", (0.05, 0.22, 0.20), (-0.295, -0.16, 0.0), accent, bev=0.015),
    ]
    assemble("head", mantle, head)

    # antenna stays on the static mount (it doesn't scan with the barrel)
    assemble("antenna", root, [
        bone_cyl("mast", 0.02, 0.012, 0.42, (0.20, -0.22, 1.0), steel, segs=8),
        sphere("tip", 0.035, (0.20, -0.22, 1.22), glow, subdiv=2),
    ])

    # --- Animation: idle = slow scanning yaw of the mantle + a subtle bob. ---
    idle = bpy.data.actions.new("idle")
    for f, yaw, bob in [(1, -32, 0.0), (45, 0, 0.012), (90, 32, 0.0),
                        (135, 0, 0.012), (180, -32, 0.0)]:
        key_rot(mantle, idle, f, (0, 0, yaw))
        key_loc(mantle, idle, f, (0, 0, MZ + bob))

    # Reset to a neutral rest pose so the exporter bakes an unrotated, centered mantle.
    if mantle.animation_data: mantle.animation_data.action = None
    mantle.rotation_euler = (0, 0, 0)
    mantle.location = Vector((0, 0, MZ))

    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(mantle, idle)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "turret.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_turret] wrote", out_path)


if __name__ == "__main__":
    main()
