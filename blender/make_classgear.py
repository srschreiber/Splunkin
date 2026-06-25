"""
Build the player CLASS GEAR and export three socket props:
  assets/models/knighthelm.glb  - knight helmet (head socket)
  assets/models/wizardhat.glb    - wizard hat   (head socket)
  assets/models/staff.glb        - wizard staff (hand socket, like sword.glb)

These are static single-model props drawn at a bone's world matrix (head / hand), the same
way helmet.glb and sword.glb are. No rig/animation. Z-up; built at human scale around the
ORIGIN (the bone), since the head/hand world matrix already places + scales them. If a prop
sits too high/low in-game, nudge the small offset constant where it's drawn in main.cpp.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_classgear.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.materials):
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

def box(name, center, size, material, rot=None):
    sx, sy, sz = size[0]/2, size[1]/2, size[2]/2
    verts = [(-sx,-sy,-sz),(sx,-sy,-sz),(sx,sy,-sz),(-sx,sy,-sz),
             (-sx,-sy,sz),(sx,-sy,sz),(sx,sy,sz),(-sx,sy,sz)]
    faces = [(0,1,2,3),(7,6,5,4),(4,5,1,0),(6,7,3,2),(5,6,2,1),(7,4,0,3)]
    m = bpy.data.meshes.new(name); m.from_pydata(verts, [], faces); m.update()
    o = bpy.data.objects.new(name, m); bpy.context.collection.objects.link(o)
    o.location = Vector(center)
    if rot: o.rotation_euler = rot
    o.data.materials.append(material)
    return o

def export(path):
    out = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", path))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_apply=False, use_selection=False)
    print("[make_classgear] wrote", out)

def knight_helm():
    reset_scene()
    steel = mat("k_steel", (0.62, 0.64, 0.70, 1.0))
    dark  = mat("k_dark", (0.34, 0.36, 0.40, 1.0))
    plume = mat("k_plume", (0.85, 0.15, 0.15, 1.0))
    # Dome around the head (origin ~ head center). Built human-scale.
    box("dome",  (0, 0, 0.06), (0.40, 0.40, 0.32), steel)
    box("brow",  (0, 0.18, 0.06), (0.42, 0.10, 0.20), dark)      # visor brow
    box("slit",  (0, 0.22, 0.06), (0.30, 0.04, 0.04), dark)      # eye slit
    box("nose",  (0, 0.20, -0.02), (0.05, 0.10, 0.18), steel)    # nasal bar
    box("crest", (0, 0, 0.26), (0.05, 0.30, 0.10), dark)         # center crest
    box("plume", (0, -0.10, 0.34), (0.08, 0.10, 0.26), plume)    # red plume
    export("knighthelm.glb")

def wizard_hat():
    reset_scene()
    blue = mat("w_hat", (0.16, 0.20, 0.62, 1.0))
    trim = mat("w_trim", (0.10, 0.12, 0.42, 1.0))
    star = mat("w_star", (0.98, 0.85, 0.25, 1.0))
    # Wide brim just above the head, then a tall tapering cone with a drooping tip.
    box("brim",  (0, 0, 0.16), (0.52, 0.52, 0.05), blue)
    box("band",  (0, 0, 0.20), (0.34, 0.34, 0.05), trim)
    box("cone1", (0, 0, 0.28), (0.28, 0.28, 0.14), blue)
    box("cone2", (0, 0.02, 0.42), (0.19, 0.19, 0.16), blue)
    box("cone3", (0, 0.06, 0.57), (0.11, 0.11, 0.16), blue)
    box("tip",   (0, 0.12, 0.70), (0.06, 0.06, 0.14), blue, rot=(math.radians(18), 0, 0))
    box("star",  (0, 0.18, 0.34), (0.07, 0.02, 0.07), star)
    export("wizardhat.glb")

def staff():
    reset_scene()
    wood = mat("s_wood", (0.45, 0.29, 0.14, 1.0))
    orb  = mat("s_orb", (0.40, 0.78, 1.0, 1.0), emissive=True)
    # A long rod along the hand's local axis with a glowing orb on top. Matches the sword's
    # rough scale/orientation (sword.glb is a long blade from the hand); tune in main if off.
    box("shaft", (0, 0, 0.0), (0.06, 0.06, 1.5), wood)
    box("grip",  (0, 0, -0.45), (0.08, 0.08, 0.18), wood)
    box("orb",   (0, 0, 0.82), (0.22, 0.22, 0.22), orb)
    box("claw0", (0.12, 0, 0.74), (0.05, 0.05, 0.16), wood, rot=(0, math.radians(35), 0))
    box("claw1", (-0.12, 0, 0.74), (0.05, 0.05, 0.16), wood, rot=(0, math.radians(-35), 0))
    export("staff.glb")

def main():
    knight_helm()
    wizard_hat()
    staff()

if __name__ == "__main__":
    main()
