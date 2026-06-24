"""
Build a defensive TURRET base/housing and export assets/models/turret.glb.

Unlike the enemies, this is a STATIC prop (no animation, no special bone names) — the game
draws it yawed toward the current target, then draws an aimed barrel procedurally on top
(so the gun still tracks/pitches in real time). So this model is just the mount + armored
housing that makes it read as a real turret.

Conventions: Z-up; ORIGIN AT GROUND (Z=0 sits on the floor — NOT the waist convention the
characters use, because turrets are placed directly at terrain height). Front faces +Y (the
barrel exits the front, so yawing the model toward the target points the housing at it).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_turret.py
"""

import bpy, os
from mathutils import Matrix, Vector

GUN   = (0.20, 0.21, 0.25, 1.0)   # dark gunmetal
STEEL = (0.46, 0.48, 0.54, 1.0)   # lighter steel
ACCENT= (0.85, 0.45, 0.15, 1.0)   # orange trim / lens

def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass

def mat(name, rgba):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    if b: b.inputs["Base Color"].default_value = rgba
    return m

def add_box(name, center, size, material, parent, rot=None):
    sx, sy, sz = size[0]/2, size[1]/2, size[2]/2
    verts = [(-sx,-sy,-sz),(sx,-sy,-sz),(sx,sy,-sz),(-sx,sy,-sz),
             (-sx,-sy,sz),(sx,-sy,sz),(sx,sy,sz),(-sx,sy,sz)]
    faces = [(0,1,2,3),(7,6,5,4),(4,5,1,0),(6,7,3,2),(5,6,2,1),(7,4,0,3)]
    m = bpy.data.meshes.new(name); m.from_pydata(verts, [], faces); m.update()
    o = bpy.data.objects.new(name, m)
    bpy.context.collection.objects.link(o)
    o.parent = parent; o.matrix_parent_inverse = Matrix.Identity(4)
    o.location = Vector(center)
    if rot: o.rotation_euler = rot
    o.data.materials.append(material)
    return o

def main():
    reset_scene()
    gun, steel, accent = mat("t_gun", GUN), mat("t_steel", STEEL), mat("t_accent", ACCENT)
    root = bpy.data.objects.new("turret", None); bpy.context.collection.objects.link(root)

    # Splayed tripod legs (three angled struts) under a round-ish base plate.
    import math
    for i in range(3):
        ang = math.radians(90 + i * 120)
        dx, dy = math.cos(ang) * 0.42, math.sin(ang) * 0.42
        leg = add_box("leg%d" % i, (dx, dy, 0.18), (0.12, 0.12, 0.42), steel, root)
        leg.rotation_euler = (math.radians(-12) * math.sin(ang), math.radians(12) * math.cos(ang), 0)
        add_box("foot%d" % i, (dx * 1.25, dy * 1.25, 0.03), (0.20, 0.20, 0.06), gun, root)

    add_box("baseplate", (0, 0, 0.34), (0.66, 0.66, 0.10), gun, root)     # hub the housing sits on
    add_box("swivel",    (0, 0, 0.44), (0.50, 0.50, 0.12), steel, root)    # rotating collar (cosmetic)

    # Armored housing (the body the gun mounts in), slightly wedge-shaped facing +Y.
    add_box("housing",   (0, 0.02, 0.66), (0.62, 0.78, 0.46), gun, root)
    add_box("brow",      (0, 0.34, 0.78), (0.52, 0.16, 0.18), steel, root)  # front brow over the barrel slot
    add_box("lens",      (0, 0.44, 0.70), (0.16, 0.05, 0.16), accent, root) # orange targeting lens on the front
    add_box("backpack",  (0, -0.34, 0.70), (0.40, 0.22, 0.34), steel, root) # rear counterweight / ammo
    add_box("ventL",     (0.30, -0.10, 0.66), (0.06, 0.34, 0.30), accent, root)  # side glow vents
    add_box("ventR",     (-0.30, -0.10, 0.66), (0.06, 0.34, 0.30), accent, root)
    add_box("antenna",   (0.18, -0.20, 1.05), (0.03, 0.03, 0.40), steel, root)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "turret.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB', export_apply=False, use_selection=False)
    print("[make_turret] wrote", out_path)

if __name__ == "__main__":
    main()
