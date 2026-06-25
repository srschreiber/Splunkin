"""
Build a large detailed GLYPH STONE for the base core and export assets/models/glyphstone.glb.
Replaces the plain cyan core column with a runed monolith.

Static prop (no rig/anim). Z-up; ORIGIN AT GROUND (Z=0); the game places/scales it at the
core position and tints the stone by the core's health (the emissive glyphs stay lit).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_glyphstone.py
"""

import bpy, math, os
from mathutils import Matrix, Vector

STONE = (0.44, 0.44, 0.49, 1.0)
DARK  = (0.30, 0.30, 0.34, 1.0)
GLYPH = (0.30, 0.92, 1.00, 1.0)   # emissive cyan runes
CORE  = (0.55, 0.95, 1.00, 1.0)   # bright emissive crystal

def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass

def mat(name, rgba, emissive=False, strength=4.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    if b:
        b.inputs["Base Color"].default_value = rgba
        if emissive and "Emission Color" in b.inputs:
            b.inputs["Emission Color"].default_value = rgba
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = strength
    return m

def box(name, parent, center, size, material, rot=None):
    sx, sy, sz = size[0]/2, size[1]/2, size[2]/2
    verts = [(-sx,-sy,-sz),(sx,-sy,-sz),(sx,sy,-sz),(-sx,sy,-sz),
             (-sx,-sy,sz),(sx,-sy,sz),(sx,sy,sz),(-sx,sy,sz)]
    faces = [(0,1,2,3),(7,6,5,4),(4,5,1,0),(6,7,3,2),(5,6,2,1),(7,4,0,3)]
    m = bpy.data.meshes.new(name); m.from_pydata(verts, [], faces); m.update()
    o = bpy.data.objects.new(name, m); bpy.context.collection.objects.link(o)
    o.parent = parent; o.matrix_parent_inverse = Matrix.Identity(4)
    o.location = Vector(center)
    if rot: o.rotation_euler = rot
    o.data.materials.append(material)
    return o

def main():
    reset_scene()
    stone, dark, glyph, core = mat("g_stone", STONE), mat("g_dark", DARK), \
        mat("g_glyph", GLYPH, emissive=True), mat("g_core", CORE, emissive=True, strength=6.0)
    root = bpy.data.objects.new("glyphstone", None); bpy.context.collection.objects.link(root)

    # Stepped plinth.
    box("plinth0", root, (0, 0, 0.12), (2.4, 2.4, 0.24), dark)
    box("plinth1", root, (0, 0, 0.34), (2.0, 2.0, 0.22), stone)
    box("plinth2", root, (0, 0, 0.52), (1.7, 1.7, 0.16), dark)

    # Tapering shaft: four stacked segments, each slightly narrower + rotated a touch.
    seg_z = [0.95, 1.75, 2.50, 3.18]
    seg_w = [1.30, 1.16, 1.02, 0.88]
    seg_h = [0.80, 0.78, 0.72, 0.64]
    for i in range(4):
        box("shaft%d" % i, root, (0, 0, seg_z[i]), (seg_w[i], seg_w[i], seg_h[i]), stone,
            rot=(0, 0, math.radians(i * 6)))
        # Emissive rune bars carved on all four faces of this segment.
        half = seg_w[i] / 2 + 0.015
        for fx, fy, sz in [(0, half, (seg_w[i]*0.5, 0.04, 0.10)), (0, -half, (seg_w[i]*0.5, 0.04, 0.10)),
                           (half, 0, (0.04, seg_w[i]*0.5, 0.10)), (-half, 0, (0.04, seg_w[i]*0.5, 0.10))]:
            box("rune%d_%d_%d" % (i, int(fx*10), int(fy*10)), root, (fx, fy, seg_z[i]+0.10), sz, glyph,
                rot=(0, 0, math.radians(i * 6)))
            box("rune%d_%d_%db" % (i, int(fx*10), int(fy*10)), root, (fx, fy, seg_z[i]-0.14), sz, glyph,
                rot=(0, 0, math.radians(i * 6)))

    # Pyramidal cap + a glowing crystal at the apex.
    box("cap0", root, (0, 0, 3.62), (0.80, 0.80, 0.22), dark)
    box("cap1", root, (0, 0, 3.82), (0.56, 0.56, 0.20), stone, rot=(0, 0, math.radians(45)))
    box("cap2", root, (0, 0, 4.00), (0.34, 0.34, 0.18), stone)
    box("crystal", root, (0, 0, 4.28), (0.26, 0.26, 0.42), core, rot=(0, 0, math.radians(45)))

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "glyphstone.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB', export_apply=False, use_selection=False)
    print("[make_glyphstone] wrote", out_path)

if __name__ == "__main__":
    main()
