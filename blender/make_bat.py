"""
Build a flying BAT enemy and export assets/models/bat.glb.

See docs/blender-model-scripting.md for the conventions; the short version (same pattern
as the rewritten make_skeleton.py — which is the gold standard this follows):

  * No GPU skinning -> rig is Empties, a separate mesh rigidly parented to each bone.
  * A glTF primitive == one (mesh, material) pair, so ALL same-material geometry parented
    to one bone is JOINED into ONE primitive == ONE draw call (assemble() bakes the
    transforms and joins). That lets us add ears/snout/fangs/struts/claws cheaply.
  * Bones the loader finds by name: body, head. wingL/wingR are extra (just animated);
    wingtipL/wingtipR are NEW child bones that curl the wing for a nicer flap arc.
  * Per-part materials DO show (draw_model does part.color * white tint), so the body is
    purple, eyes glow red, fangs are bone-white, etc.
  * Clip "walk" = the wing flap (a flyer plays it continuously in flight).
  * Clip "idle" = a slower hover (gentle flap + body bob + head turn).
  * Origin at the body center (flyers render at hover height, no foot lift).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_bat.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

PURPLE = (0.50, 0.18, 0.72, 1.0)   # furry body / head / ears
MEMBR  = (0.34, 0.10, 0.46, 1.0)   # darker wing membrane
STRUT  = (0.62, 0.30, 0.82, 1.0)   # lighter wing-finger struts (the "veins")
BLACK  = (0.02, 0.02, 0.03, 1.0)   # eye sockets
RED    = (0.95, 0.12, 0.12, 1.0)   # glowing eyes
PINK   = (0.85, 0.45, 0.62, 1.0)   # ear insides / nose
BONE   = (0.92, 0.90, 0.84, 1.0)   # fangs + claws


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


# ---- smooth rounded primitive builders (mirrors make_skeleton.py) -------------------
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

def box(name, size, loc, mat_, rot=None, bev=0.012, smooth=False):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), smooth)


def assemble(name, bone, objs):
    """Bake transforms + JOIN same-material geometry on a bone into ONE primitive."""
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
    purple = mat("bat_body", PURPLE)
    membr  = mat("bat_membrane", MEMBR)
    strut  = mat("bat_strut", STRUT)
    black  = mat("bat_eyesocket", BLACK)
    red    = mat("bat_eye", RED, emissive=True)
    pink   = mat("bat_ear", PINK)
    boneM  = mat("bat_fang", BONE)

    # --- Rig. Forward is +Y. Body at origin; head toward +Y; wings out along X. ---
    body   = make_empty("body",  None, (0, 0, 0))
    head   = make_empty("head",  body, (0, 0.26, 0.14))
    wingL  = make_empty("wingL", body, (0.16, 0, 0.06))
    wingR  = make_empty("wingR", body, (-0.16, 0, 0.06))
    wingtipL = make_empty("wingtipL", wingL, (0.40, 0, 0.0))   # NEW: curls the outer wing
    wingtipR = make_empty("wingtipR", wingR, (-0.40, 0, 0.0))

    # --- BODY (waist bone): a rounded furry torso + belly + chest tuft + shoulder humps.
    #     All purple -> one primitive. Little tucked legs join too. Claws are a 2nd prim.
    body_purple = [
        sphere("torso", 0.19, (0, 0.0, 0.0),  purple, scale=(0.95, 1.25, 1.05), subdiv=3),
        sphere("belly", 0.15, (0, 0.06, -0.07), purple, scale=(0.95, 1.0, 0.9), subdiv=2),
        sphere("ruff",  0.14, (0, -0.14, 0.06), purple, scale=(1.05, 0.7, 1.05), subdiv=2),  # furry scruff
        sphere("shldL", 0.085, (0.15, -0.02, 0.10), purple, subdiv=2),  # wing-root humps
        sphere("shldR", 0.085, (-0.15, -0.02, 0.10), purple, subdiv=2),
        # tucked little legs (flyer keeps them folded under the body)
        bone_cyl("legL", 0.03, 0.04, 0.16, (0.08, 0.12, -0.20), purple, rot=(60, 0, 0), segs=8),
        bone_cyl("legR", 0.03, 0.04, 0.16, (-0.08, 0.12, -0.20), purple, rot=(60, 0, 0), segs=8),
    ]
    assemble("body_mesh", body, body_purple)
    # clawed feet (bone-white) -> one primitive
    claws = []
    for s in (1.0, -1.0):
        for k in range(3):
            cx = 0.08 * s + (k - 1) * 0.03
            claws.append(bone_cyl("claw%d%d" % (int(s), k), 0.012, 0.0, 0.06,
                                  (cx, 0.18, -0.27), boneM, rot=(70, 0, 0), segs=5))
    assemble("claws", body, claws)

    # --- HEAD (head bone). Smooth cranium + snout + brow + tall pointed ears. ---
    assemble("skull", head, [
        sphere("cranium", 0.155, (0, 0.0, 0.02),  purple, scale=(1.05, 1.0, 1.0), subdiv=3),
        sphere("snout",   0.10,  (0, 0.13, -0.05), purple, scale=(0.85, 1.05, 0.8), subdiv=2),
        box("brow", (0.20, 0.05, 0.045), (0, 0.10, 0.10), purple, bev=0.015, smooth=True),
        # tall pointed ears (taper to a tip)
        bone_cyl("earL", 0.06, 0.005, 0.26, (0.09, -0.03, 0.22), purple, rot=(-12, 0, -8), segs=10),
        bone_cyl("earR", 0.06, 0.005, 0.26, (-0.09, -0.03, 0.22), purple, rot=(-12, 0, 8), segs=10),
    ])
    # pink ear insides + nose
    assemble("ear_in", head, [
        bone_cyl("earInL", 0.032, 0.004, 0.20, (0.09, -0.01, 0.21), pink, rot=(-12, 0, -8), segs=8),
        bone_cyl("earInR", 0.032, 0.004, 0.20, (-0.09, -0.01, 0.21), pink, rot=(-12, 0, 8), segs=8),
        sphere("nose", 0.028, (0, 0.205, -0.05), pink, subdiv=2),
    ])
    # black eye sockets
    assemble("sockets", head, [
        sphere("socketL", 0.05, (0.075, 0.135, 0.04), black, scale=(1.0, 0.8, 1.1), subdiv=2),
        sphere("socketR", 0.05, (-0.075, 0.135, 0.04), black, scale=(1.0, 0.8, 1.1), subdiv=2),
    ])
    # glowing red eyes
    assemble("eyes", head, [
        sphere("eyeL", 0.03, (0.075, 0.165, 0.045), red, subdiv=2),
        sphere("eyeR", 0.03, (-0.075, 0.165, 0.045), red, subdiv=2),
    ])
    # bone-white fangs (point down out of the snout)
    assemble("fangs", head, [
        bone_cyl("fangL", 0.018, 0.0, 0.08, (0.045, 0.16, -0.12), boneM, rot=(20, 0, 0), segs=6),
        bone_cyl("fangR", 0.018, 0.0, 0.08, (-0.045, 0.16, -0.12), boneM, rot=(20, 0, 0), segs=6),
    ])

    # --- WINGS: ONE simple clean membrane per side — an inner panel on the wing bone and an
    #     outer panel on the wingtip bone (so the tip curls through the flap). No finger-struts
    #     or doubled panels: those read as frayed/extra wings. Just a smooth bat-wing sheet. ---
    def wing(side, root_bone, tip_bone):
        s = 1.0 if side == "L" else -1.0
        assemble("wingmem%s" % side, root_bone, [
            box("wmem%s" % side, (0.46, 0.52, 0.02), (0.22 * s, -0.02, 0.0), membr, bev=0.04, smooth=True),
        ])
        assemble("wingtipmem%s" % side, tip_bone, [
            box("wtmem%s" % side, (0.34, 0.42, 0.018), (0.15 * s, -0.05, 0.0), membr, bev=0.04, smooth=True),
        ])
    wing("L", wingL, wingtipL)
    wing("R", wingR, wingtipR)

    # --- ANIMATIONS -------------------------------------------------------------------
    # "walk" = energetic continuous flap. Wings sweep about Y; the wingtip lags + curls
    #          for a membranous arc; the body bobs slightly with each downbeat.
    walk = bpy.data.actions.new("walk")
    for f, a in [(1, 8), (5, 48), (11, -28), (15, 8)]:        # ~15-frame loop, wingL Y angle
        key_rot(wingL, walk, f, (0, a, 0))
        key_rot(wingR, walk, f, (0, -a, 0))
    for f, a in [(1, 6), (5, 34), (11, -20), (15, 6)]:        # tip curls a touch more, lagging
        key_rot(wingtipL, walk, f, (0, a, 0))
        key_rot(wingtipR, walk, f, (0, -a, 0))
    for f, z in [(1, 0.0), (5, 0.035), (11, -0.02), (15, 0.0)]:
        key_loc(body, walk, f, (0, 0, z))

    # "idle" = slower hover: gentler flap, a smooth up/down bob, and a lazy head sweep.
    idle = bpy.data.actions.new("idle")
    for f, a in [(1, 6), (20, 26), (40, -10), (60, 6)]:
        key_rot(wingL, idle, f, (0, a, 0))
        key_rot(wingR, idle, f, (0, -a, 0))
    for f, a in [(1, 4), (20, 18), (40, -8), (60, 4)]:
        key_rot(wingtipL, idle, f, (0, a, 0))
        key_rot(wingtipR, idle, f, (0, -a, 0))
    for f, z in [(1, 0.0), (20, 0.05), (40, -0.01), (60, 0.0)]:
        key_loc(body, idle, f, (0, 0, z))
    for f, yaw, pitch in [(1, 0, 0), (20, 14, 4), (40, -14, -3), (60, 0, 0)]:
        key_rot(head, idle, f, (pitch, 0, yaw))

    # Park clips on NLA tracks (per object) so ACTIONS export emits both, and reset every
    # animated bone to its REST transform so the exporter bakes a neutral rest pose.
    anim_objs = (wingL, wingR, wingtipL, wingtipR, body, head)
    for o in anim_objs:
        if o.animation_data: o.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    for o in (wingL, wingR, wingtipL, wingtipR, body):
        stash(o, walk)
    for o in anim_objs:
        stash(o, idle)
    # neutral rest pose
    for o in anim_objs:
        o.rotation_euler = (0, 0, 0)
    body.location = Vector((0, 0, 0))

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "bat.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_bat] wrote", out_path)


if __name__ == "__main__":
    main()
