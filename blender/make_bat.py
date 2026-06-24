"""
Build a flying BAT enemy (purple body, black eyes, flapping wings) and export
assets/models/bat.glb.  See docs/blender-model-scripting.md for the conventions; the
short version (same as make_skeleton.py):

  * No GPU skinning -> rig is Empties, a separate mesh box parented to each bone.
  * Bones the loader finds by name: body, head (wingL/wingR are extra, just animated).
  * Per-part materials DO show (draw_model does part.color * tint, drawn with white tint),
    so the body can be purple and the eyes black.
  * Animation named "walk" = the wing flap (a flyer plays it continuously).
  * Origin at the body center (flyers render at hover height, no foot lift).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_bat.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

PURPLE = (0.50, 0.18, 0.72, 1.0)
MEMBR  = (0.34, 0.10, 0.46, 1.0)   # darker wing membrane
STRUT  = (0.62, 0.30, 0.82, 1.0)   # lighter wing-finger struts (the "veins")
BLACK  = (0.02, 0.02, 0.03, 1.0)
RED    = (0.95, 0.12, 0.12, 1.0)   # glowing eyes
PINK   = (0.85, 0.45, 0.62, 1.0)   # ear insides

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
    purple = mat("bat_body", PURPLE)
    membr  = mat("bat_membrane", MEMBR)
    strut  = mat("bat_strut", STRUT)
    black  = mat("bat_eyesocket", BLACK)
    red    = mat("bat_eye", RED, emissive=True)
    pink   = mat("bat_ear", PINK)

    # Forward is +Y. Body centered at origin; wings out along X; head toward +Y.
    body  = make_empty("body",  None, (0, 0, 0))
    head  = make_empty("head",  body, (0, 0.3, 0.12))
    wingL = make_empty("wingL", body, (0.18, 0, 0.05))
    wingR = make_empty("wingR", body, (-0.18, 0, 0.05))

    make_part("torso",  body, (0, 0, 0),        (0.34, 0.5, 0.30), purple)   # body
    make_part("skull",  head, (0, 0.04, 0),     (0.26, 0.24, 0.22), purple)  # head
    # Ears: outer purple shell + pink inner.
    make_part("earL",   head, (0.09, -0.02, 0.16), (0.05, 0.04, 0.16), purple)
    make_part("earR",   head, (-0.09, -0.02, 0.16),(0.05, 0.04, 0.16), purple)
    make_part("earInL", head, (0.09, 0.0, 0.15),   (0.025, 0.03, 0.11), pink)
    make_part("earInR", head, (-0.09, 0.0, 0.15),  (0.025, 0.03, 0.11), pink)
    # Eyes: black socket + glowing red pupil.
    make_part("socketL",head, (0.07, 0.15, 0.02),  (0.08, 0.04, 0.08), black)
    make_part("socketR",head, (-0.07, 0.15, 0.02), (0.08, 0.04, 0.08), black)
    make_part("eyeL",   head, (0.07, 0.17, 0.02),  (0.045, 0.03, 0.045), red)
    make_part("eyeR",   head, (-0.07, 0.17, 0.02), (0.045, 0.03, 0.045), red)
    make_part("fangL",  head, (0.04, 0.13, -0.13), (0.025, 0.03, 0.06), pink)
    make_part("fangR",  head, (-0.04, 0.13, -0.13),(0.025, 0.03, 0.06), pink)
    # Wings: dark membrane panel + lighter struts (wing fingers) reading as veins.
    def wing(side, bone):
        s = 1.0 if side == "L" else -1.0
        make_part("wing%s" % side, bone, (0.34 * s, 0, 0), (0.62, 0.42, 0.03), membr)
        for i, (dx, dz, ln, ang) in enumerate([(0.12, 0.10, 0.5, 18), (0.18, 0.0, 0.58, 0), (0.14, -0.12, 0.5, -20)]):
            o = make_part("strut%s%d" % (side, i), bone, (dx * s, 0.02, dz), (ln, 0.05, 0.05), strut)
            o.rotation_mode = 'XYZ'; o.rotation_euler = (0, math.radians(ang * s), 0)
    wing("L", wingL)
    wing("R", wingR)

    # "walk" = a fast wing flap (a flyer plays it continuously). Rotate wings about Y
    # (forward axis) so the membranes sweep up and down; mirrored so they flap together.
    flap = bpy.data.actions.new("walk")
    seq = [(1, 0), (5, 45), (11, -25), (15, 0)]   # ~15-frame loop, wingL Y angle
    for f, a in seq:
        key_rot(wingL, flap, f, (0, a, 0))
        key_rot(wingR, flap, f, (0, -a, 0))

    for o in (wingL, wingR):
        o.animation_data.action = None
        tr = o.animation_data.nla_tracks.new(); tr.name = "walk"
        tr.strips.new("walk", int(flap.frame_range[0]), flap)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "bat.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_bat] wrote", out_path)

if __name__ == "__main__":
    main()
