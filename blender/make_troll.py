"""
Build a TROLL enemy (big, hunched, wielding a stone-pillar club) and export
assets/models/troll.glb. Behavior (in the engine): slow, huge HP, big telegraphed swing
with massive damage + knockback. See docs/blender-model-scripting.md.

Rig: Empties body/head/armL/armR/handL/handR (+ legL/legR). Clips: walk, punch. The punch
swings the LEFT arm (the engine masks the enemy punch clip to armL) so the CLUB is held in
the LEFT hand. Z-up, front +Y, feet near Z=-1.0. The game renders it scaled up (big).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_troll.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

SKIN  = (0.40, 0.50, 0.36, 1.0)   # grey-green hide
DARK  = (0.30, 0.38, 0.28, 1.0)   # darker hide
LOIN  = (0.36, 0.26, 0.16, 1.0)   # leather loincloth
NAIL  = (0.85, 0.84, 0.78, 1.0)   # tusks / nails
STONE = (0.50, 0.50, 0.54, 1.0)   # club pillar
DSTONE= (0.38, 0.38, 0.42, 1.0)
EYE   = (0.95, 0.75, 0.15, 1.0)   # yellow eyes (emissive)

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
    skin, dark, loin, nail, stone, dstone, eye = (
        mat("t_skin", SKIN), mat("t_dark", DARK), mat("t_loin", LOIN), mat("t_nail", NAIL),
        mat("t_stone", STONE), mat("t_dstone", DSTONE), mat("t_eye", EYE, emissive=True))

    # Hunched: torso leans forward (+Y). Big shoulders, long arms.
    body  = make_empty("body",  None, (0, 0, 0.0))
    head  = make_empty("head",  body, (0, 0.12, 0.74))
    armL  = make_empty("armL",  body, (0.42, 0.04, 0.66))   # holds the club
    armR  = make_empty("armR",  body, (-0.42, 0.04, 0.66))
    handL = make_empty("handL", armL, (0, 0, -0.58))
    handR = make_empty("handR", armR, (0, 0, -0.58))
    legL  = make_empty("legL",  body, (0.18, 0, 0.0))
    legR  = make_empty("legR",  body, (-0.18, 0, 0.0))

    # Bulky torso + hunched back + leather loin.
    make_part("chest",   body, (0, 0.04, 0.42), (0.78, 0.52, 0.66), skin)
    make_part("hump",    body, (0, -0.16, 0.62), (0.5, 0.34, 0.36), dark)   # hunched back
    make_part("gut",     body, (0, 0.08, 0.10), (0.66, 0.50, 0.40), skin)
    make_part("loin",    body, (0, 0.06, -0.18), (0.62, 0.44, 0.30), loin)
    make_part("shoulderL", body, (0.40, 0.02, 0.66), (0.34, 0.34, 0.30), dark)
    make_part("shoulderR", body, (-0.40, 0.02, 0.66), (0.34, 0.34, 0.30), dark)

    # Head: heavy brow, yellow eyes, two tusks.
    make_part("skull", head, (0, 0.04, 0.04), (0.40, 0.40, 0.36), skin)
    make_part("brow",  head, (0, 0.20, 0.10), (0.40, 0.10, 0.10), dark)
    make_part("eyeL",  head, (0.10, 0.20, 0.06), (0.06, 0.04, 0.06), eye)
    make_part("eyeR",  head, (-0.10, 0.20, 0.06), (0.06, 0.04, 0.06), eye)
    make_part("tuskL", head, (0.09, 0.18, -0.14), (0.04, 0.05, 0.14), nail)
    make_part("tuskR", head, (-0.09, 0.18, -0.14), (0.04, 0.05, 0.14), nail)

    # Arms (thick). Legs (stubby, strong).
    make_part("uarmL", armL, (0, 0, -0.30), (0.20, 0.20, 0.60), skin)
    make_part("uarmR", armR, (0, 0, -0.30), (0.20, 0.20, 0.60), skin)
    make_part("fistL", handL, (0, 0, -0.06), (0.22, 0.22, 0.22), skin)
    make_part("fistR", handR, (0, 0, -0.06), (0.22, 0.22, 0.22), skin)
    make_part("thighL", legL, (0, 0, -0.34), (0.26, 0.26, 0.5), skin)
    make_part("thighR", legR, (0, 0, -0.34), (0.26, 0.26, 0.5), skin)
    make_part("footL", legL, (0, 0.06, -0.92), (0.28, 0.40, 0.20), dark)
    make_part("footR", legR, (0, 0.06, -0.92), (0.28, 0.40, 0.20), dark)

    # Stone-pillar CLUB held in the LEFT hand (so the masked-armL punch swings it).
    make_part("clubshaft", handL, (0, 0, -0.85), (0.26, 0.26, 1.30), stone)
    make_part("clubhead",  handL, (0, 0, -1.55), (0.40, 0.40, 0.50), dstone)
    for k in range(4):
        a = math.radians(k * 90)
        make_part("spike%d" % k, handL, (math.cos(a)*0.26, math.sin(a)*0.26, -1.55), (0.12, 0.12, 0.16), dstone,
                  rot=(0, 0, a))

    # walk: slow heavy lumber.
    walk = bpy.data.actions.new("walk")
    for frame, a in [(1, 16), (16, -16), (31, 16)]:   # slower, wide stride (longer loop)
        key_rot(legL, walk, frame, (a, 0, 0))
        key_rot(legR, walk, frame, (-a, 0, 0))
        key_rot(armR, walk, frame, (-a*0.5, 0, 0))
        key_rot(armL, walk, frame, (a*0.3, 0, 0))     # club arm swings a little

    # punch: a BIG overhead club smash with WEIGHT — the torso winds back then lurches into
    # the slam, the legs counterbalance, and the left arm whips the club down.
    punch = bpy.data.actions.new("punch")
    # The engine only plays this clip DURING the wind-up (attacking flips off at the damage
    # frame), sampled NORMALIZED against TROLL_WINDUP — so the SLAM CONTACT must be the FINAL
    # keyframe. That lands the club-down pose exactly on the damage moment (no early/late slam).
    for frame, a in [(1, 0), (11, -120), (30, 85)]:              # armL: raise back -> slam CONTACT at end
        key_rot(armL, punch, frame, (a, 0, 0))
    # torso: lean back + twist on windup, then lurch forward + untwist into the slam (weight).
    for frame, lean, twist in [(1, 0, 0), (11, -22, 14), (30, 30, -16)]:
        if not body.animation_data: body.animation_data_create()
        body.animation_data.action = punch; body.rotation_mode = 'XYZ'
        body.rotation_euler = (math.radians(lean), 0, math.radians(twist))
        body.keyframe_insert(data_path="rotation_euler", frame=frame)
    for frame, l in [(1, 0), (11, 18), (30, -22)]:                # legs counterbalance the swing's weight
        key_rot(legL, punch, frame, (l, 0, 0))
        key_rot(legR, punch, frame, (l, 0, 0))

    for obj in (legL, legR, armL, armR, body):
        if obj.animation_data: obj.animation_data.action = None
    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(legL, walk); stash(legR, walk); stash(armR, walk)
    stash(armL, walk); stash(armL, punch)
    stash(legL, punch); stash(legR, punch); stash(body, punch)   # full-body weighted swing

    # The punch clip now ENDS on the slam (non-neutral). Reset every bone to neutral and park
    # on frame 1 so the exporter bakes an UPRIGHT rest pose (not the slammed pose).
    for obj in (legL, legR, armL, armR, body, head):
        obj.rotation_euler = (0, 0, 0)
    bpy.context.scene.frame_set(1)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "troll.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_troll] wrote", out_path)

if __name__ == "__main__":
    main()
