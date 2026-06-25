"""
Build a DEMON enemy (big, horned, winged, glowing) and export assets/models/demon.glb.
Behavior (engine): big + strong; hurls slow fireballs that EXPLODE on impact (rocket-launcher
style splash). See docs/blender-model-scripting.md.

Rig: Empties body/head/armL/armR/handL/handR (+ legL/legR, wingL/wingR). Clips: walk (stomp +
wing flap), punch (cast). Z-up, front +Y, feet near Z=-1.0. The game renders it scaled up.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_demon.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

RED   = (0.60, 0.13, 0.10, 1.0)
DARK  = (0.32, 0.07, 0.06, 1.0)
HORN  = (0.10, 0.09, 0.11, 1.0)
FIRE  = (1.00, 0.50, 0.10, 1.0)   # emissive glow (eyes, chest, hands)
WING  = (0.26, 0.06, 0.07, 1.0)   # wing membrane
HOOF  = (0.12, 0.10, 0.10, 1.0)

def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass

def mat(name, rgba, emissive=False, strength=3.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    if b:
        b.inputs["Base Color"].default_value = rgba
        if emissive and "Emission Color" in b.inputs:
            b.inputs["Emission Color"].default_value = rgba
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = strength
    return m

def make_empty(name, parent, loc, rot=(0,0,0)):
    e = bpy.data.objects.new(name, None); e.empty_display_size = 0.1
    bpy.context.collection.objects.link(e)
    if parent: e.parent = parent
    e.location = Vector(loc); e.rotation_euler = rot
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

def key_rot(obj, action, frame, euler_deg):
    if not obj.animation_data: obj.animation_data_create()
    obj.animation_data.action = action
    obj.rotation_mode = 'XYZ'
    obj.rotation_euler = tuple(math.radians(a) for a in euler_deg)
    obj.keyframe_insert(data_path="rotation_euler", frame=frame)

def main():
    reset_scene()
    red, dark, horn, fire, wing, hoof = (
        mat("d_red", RED), mat("d_dark", DARK), mat("d_horn", HORN),
        mat("d_fire", FIRE, emissive=True), mat("d_wing", WING), mat("d_hoof", HOOF))

    body  = make_empty("body",  None, (0, 0, 0.0))
    head  = make_empty("head",  body, (0, 0, 0.82))
    armL  = make_empty("armL",  body, (0.38, 0, 0.74))
    armR  = make_empty("armR",  body, (-0.38, 0, 0.74))
    handL = make_empty("handL", armL, (0, 0, -0.52))
    handR = make_empty("handR", armR, (0, 0, -0.52))
    legL  = make_empty("legL",  body, (0.16, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.16, 0, 0.0))
    wingL = make_empty("wingL", body, (0.20, -0.18, 0.62))
    wingR = make_empty("wingR", body, (-0.20, -0.18, 0.62))

    # Torso: broad chest with a glowing fire core; muscled abdomen.
    make_part("chest", body, (0, 0.02, 0.52), (0.70, 0.42, 0.56), red)
    make_part("abs",   body, (0, 0.04, 0.16), (0.54, 0.38, 0.40), red)
    make_part("pelvis",body, (0, 0, -0.16), (0.52, 0.36, 0.26), dark)
    make_part("pecL",  body, (0.18, 0.16, 0.62), (0.26, 0.20, 0.22), red)
    make_part("pecR",  body, (-0.18, 0.16, 0.62), (0.26, 0.20, 0.22), red)

    # Head: horned skull, glowing eyes. The JAW is its own bone (hinged at the back) so the
    # punch clip can open the mouth for the "yell". A glowing maw lights up inside.
    jaw = make_empty("jaw", head, (0, -0.06, -0.04))   # hinge near the back of the head
    make_part("skull", head, (0, 0.02, 0.06), (0.36, 0.34, 0.34), red)
    make_part("jawmesh", jaw, (0, 0.14, -0.06), (0.30, 0.26, 0.12), dark)   # lower jaw (swings open)
    make_part("maw",     jaw, (0, 0.10, -0.02), (0.20, 0.16, 0.06), fire)   # glowing throat
    make_part("eyeL",  head, (0.08, 0.16, 0.10), (0.06, 0.04, 0.05), fire)
    make_part("eyeR",  head, (-0.08, 0.16, 0.10), (0.06, 0.04, 0.05), fire)
    for s, sx in [("L", 1), ("R", -1)]:
        make_part("horn%s0" % s, head, (sx*0.14, -0.04, 0.22), (0.08, 0.08, 0.20), horn)
        make_part("horn%s1" % s, head, (sx*0.20, -0.12, 0.36), (0.06, 0.06, 0.18), horn,
                  rot=(math.radians(-30), 0, 0))

    # Arms with glowing hands (where fireballs gather); clawed.
    make_part("uarmL", armL, (0, 0, -0.26), (0.16, 0.16, 0.52), red)
    make_part("uarmR", armR, (0, 0, -0.26), (0.16, 0.16, 0.52), red)
    make_part("handL", handL, (0, 0.02, -0.05), (0.16, 0.16, 0.16), fire)   # fire gathers here
    make_part("handR", handR, (0, 0.02, -0.05), (0.16, 0.16, 0.16), fire)

    # Hooved legs.
    make_part("thighL", legL, (0, 0, -0.34), (0.22, 0.22, 0.52), red)
    make_part("thighR", legR, (0, 0, -0.34), (0.22, 0.22, 0.52), red)
    make_part("shinL", legL, (0, -0.04, -0.78), (0.16, 0.16, 0.42), dark)
    make_part("shinR", legR, (0, -0.04, -0.78), (0.16, 0.16, 0.42), dark)
    make_part("hoofL", legL, (0, 0.06, -1.02), (0.18, 0.26, 0.16), hoof)
    make_part("hoofR", legR, (0, 0.06, -1.02), (0.18, 0.26, 0.16), hoof)

    # Bat-like wings (membrane + a couple of struts), trailing -Y.
    def make_wing(side, bone):
        s = 1.0 if side == "L" else -1.0
        make_part("wing%s" % side, bone, (s*0.45, -0.20, 0.10), (0.85, 0.55, 0.04), wing,
                  rot=(0, 0, math.radians(-20*s)))
        make_part("wingbone%s" % side, bone, (s*0.42, -0.10, 0.20), (0.80, 0.06, 0.07), dark,
                  rot=(0, 0, math.radians(-20*s)))
    make_wing("L", wingL); make_wing("R", wingR)

    # walk: heavy stomp + slow wing flap.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 18), (16, -18), (31, 18)]:
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armL, walk, frame, (-a*0.4, 0, 0))
        key_rot(armR, walk, frame, (a*0.4, 0, 0))
    for frame, w in [(1, 8), (16, -14), (31, 8)]:
        key_rot(wingL, walk, frame, (0, w, 0))
        key_rot(wingR, walk, frame, (0, -w, 0))

    # punch = the YELL: head tilts back, jaw gapes open (glowing maw), arm hurls the fireball.
    punch = bpy.data.actions.new("punch")
    for frame, a in [(1, 0), (7, -70), (13, 30), (20, 0)]:
        key_rot(armL, punch, frame, (a, 0, 0))
    for frame, j in [(1, 0), (7, 12), (13, 55), (20, 0)]:        # jaw opens wide on the yell
        key_rot(jaw, punch, frame, (j, 0, 0))
    for frame, h in [(1, 0), (7, -8), (13, -20), (20, 0)]:        # head rears back
        key_rot(head, punch, frame, (h, 0, 0))

    for obj in (legL, legR, armL, armR, wingL, wingR, jaw, head):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    for obj in (legL, legR, armR, wingL, wingR): stash(obj, walk)
    stash(armL, walk); stash(armL, punch); stash(jaw, punch); stash(head, punch)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "demon.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_demon] wrote", out_path)

if __name__ == "__main__":
    main()
