"""
Build a hovering quadcopter DRONE (scavenger / recon flyer) and export
assets/models/drone.glb. See docs/blender-model-scripting.md.

REVAMP: smooth rounded fuselage (icosphere capsule), tapered arms, rounded rotor hubs with
slimmer blades, a camera gimbal ball on the nose, and landing skids. Detail is added cheaply
by JOINing same-material geometry on a bone into one primitive (one draw call), exactly like
make_skeleton.py.

ENGINE FACTS (docs/blender-model-scripting.md):
  * No GPU skinning. Each glTF primitive is a rigid "part" on the NODE holding the mesh, drawn
    at placement * partNodeWorld. So the rig is a hierarchy of EMPTIES with meshes parented on.
  * A glTF primitive == one (mesh, material) pair, so all same-material geometry on a bone
    collapses to one primitive. assemble() bakes transforms + joins per bone.
  * Clips routed by name. The engine ALWAYS plays the drone's spin clip named "walk" to whirl
    the props (main.cpp). We KEEP that name. We ADD an "idle" clip (gentle hover wobble + slow
    gimbal turn + props still turning subtly) for when the drone is parked.

Rig (Empties): drone(root) -> chassis -> { gimbal, rotor0..3 }
  chassis wobbles in idle; gimbal (camera ball) pans/tilts; rotor0..3 are the spinning hubs.
Z-up, front +Y, ORIGIN AT CENTER (drones hover, no foot lift). Per-part materials show under
the engine's white tint.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_drone.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

BODY  = (0.26, 0.28, 0.34, 1.0)   # steel body
TRIM  = (0.30, 0.50, 0.92, 1.0)   # blue trim
LENS  = (0.95, 0.20, 0.15, 1.0)   # red eye (emissive)
ROTOR = (0.14, 0.14, 0.17, 1.0)   # rotor hub / arms / skids
BLADE = (0.62, 0.64, 0.70, 1.0)   # propeller blades (light = motion-blur read)


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

def box(name, size, loc, mat_, rot=None, bev=0.01):
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


def main():
    reset_scene()
    body, trim, lens, rotor, blade = (
        mat("d_body", BODY), mat("d_trim", TRIM), mat("d_lens", LENS, emissive=True),
        mat("d_rotor", ROTOR), mat("d_blade", BLADE))

    # --- Rig (empties). FRONT faces +Y. ---
    root    = make_empty("drone",   None, (0, 0, 0))
    chassis = make_empty("chassis", root, (0, 0, 0))
    gimbal  = make_empty("gimbal",  chassis, (0, 0.17, -0.05))

    HZ = 0.085
    corners = [(0.26, 0.26), (-0.26, 0.26), (0.26, -0.26), (-0.26, -0.26)]

    # --- CHASSIS: smooth oblong fuselage (icospheres) + trim canopy + red eye + tapered arms
    # + tube landing skids. All ride the chassis bone so the whole craft wobbles in idle. ---
    parts = [
        sphere("fuselage", 0.165, (0, 0.00, 0.0),  body, scale=(0.95, 1.45, 0.70), subdiv=3),
        sphere("nose",     0.105, (0, 0.16, -0.01), body, scale=(1.0, 1.1, 0.78), subdiv=2),
        sphere("tail",     0.095, (0, -0.16, 0.01), body, scale=(1.0, 1.0, 0.8), subdiv=2),
        sphere("belly",    0.12,  (0, 0.0, -0.05),  body, scale=(1.0, 1.25, 0.55), subdiv=2),
        sphere("canopy",   0.115, (0, -0.01, 0.055), trim, scale=(1.0, 1.35, 0.5), subdiv=2),
        box("band",  (0.30, 0.20, 0.035), (0, 0, 0.02), trim, bev=0.015),
        sphere("eye",      0.05,  (0, 0.235, -0.005), lens, scale=(1.1, 0.7, 1.0), subdiv=2),
    ]
    # Tapered arms reaching out to each rotor + a motor pod at the tip.
    for i, (ax, ay) in enumerate(corners):
        th = math.degrees(math.atan2(ay, ax))
        L = math.hypot(ax, ay)
        mx, my = ax * 0.52, ay * 0.52
        parts.append(bone_cyl("arm%d" % i, 0.05, 0.032, L * 1.02, (mx, my, 0.02),
                              rotor, rot=(90, 0, th - 90), segs=8))
        parts.append(bone_cyl("motor%d" % i, 0.052, 0.052, 0.07, (ax, ay, 0.04), rotor, segs=10))
    # Tube landing skids: two fore-aft rails on curved struts.
    for sx in (0.11, -0.11):
        parts.append(bone_cyl("skid_%d" % int(sx * 100), 0.022, 0.022, 0.30,
                              (sx, 0, -0.135), rotor, rot=(90, 0, 0), segs=8))
        parts.append(bone_cyl("strutF_%d" % int(sx * 100), 0.016, 0.016, 0.10,
                              (sx, 0.10, -0.085), rotor, rot=(20, 0, 0), segs=6))
        parts.append(bone_cyl("strutB_%d" % int(sx * 100), 0.016, 0.016, 0.10,
                              (sx, -0.10, -0.085), rotor, rot=(-20, 0, 0), segs=6))
    assemble("body_mesh", chassis, parts)

    # --- GIMBAL: a rounded camera ball under the nose with a glowing red lens. Pans in idle. ---
    assemble("camera", gimbal, [
        sphere("gball", 0.062, (0, 0, 0), rotor, subdiv=2),
        sphere("glens", 0.03,  (0, 0.05, 0), lens, scale=(1.0, 0.6, 1.0), subdiv=2),
    ])

    # --- ROTORS: rounded hub cap + two slim crossed blades on each spinning hub. ---
    hubs = []
    for i, (ax, ay) in enumerate(corners):
        hub = make_empty("rotor%d" % i, chassis, (ax, ay, HZ))
        assemble("prop%d" % i, hub, [
            sphere("hubcap%d" % i, 0.04, (0, 0, 0.012), rotor, scale=(1.0, 1.0, 0.7), subdiv=2),
            box("bladeA%d" % i, (0.32, 0.034, 0.009), (0, 0, 0.028), blade, bev=0.004),
            box("bladeB%d" % i, (0.034, 0.32, 0.009), (0, 0, 0.028), blade, bev=0.004),
        ])
        hubs.append(hub)

    # --- Animations ---
    # walk = whirl every rotor about Z (full turn over an 8-frame loop), phase-offset. KEPT NAME.
    walk = bpy.data.actions.new("walk")
    for i, hub in enumerate(hubs):
        for f, deg in [(1, 0), (5, 180), (9, 360)]:
            key_rot(hub, walk, f, (0, 0, deg + i * 30))

    # idle = gentle hover: chassis bobs + banks slowly, the gimbal pans/tilts to scan, and the
    # props keep turning subtly (a slow quarter-ish drift so a parked drone still feels alive).
    idle = bpy.data.actions.new("idle")
    for f, rise, bank, pitch in [(1, 0.0, 0.0, 0.0), (31, 0.03, 2.5, 1.5), (61, 0.0, 0.0, 0.0)]:
        key_loc(chassis, idle, f, (0, 0, rise))
        key_rot(chassis, idle, f, (pitch, bank, 0))
    for f, yaw, tilt in [(1, -18, 4), (31, 18, -6), (61, -18, 4)]:
        key_rot(gimbal, idle, f, (tilt, 0, yaw))
    for i, hub in enumerate(hubs):
        for f, deg in [(1, 0), (61, 120 + i * 20)]:
            key_rot(hub, idle, f, (0, 0, deg))

    # Reset to a neutral rest pose so the exporter bakes an untilted/centered drone.
    for obj in (chassis, gimbal, *hubs):
        if obj.animation_data: obj.animation_data.action = None
        obj.rotation_euler = (0, 0, 0)
    chassis.location = Vector((0, 0, 0))

    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    for hub in hubs: stash(hub, walk)
    stash(chassis, idle); stash(gimbal, idle)
    for hub in hubs: stash(hub, idle)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "drone.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_drone] wrote", out_path)


if __name__ == "__main__":
    main()
