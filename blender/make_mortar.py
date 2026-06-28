"""
Procedurally build a base-defense MORTAR prop and export assets/models/mortar.glb.

A stout, heavy siege MORTAR: a thick, steeply-elevated tapered iron tube with a hollow
muzzle (dark bore disk), wrapped in iron reinforcing bands, sitting in a heavy timber
carriage. The barrel pivots between two cheek plates on brass trunnion caps; two small
wheels and a loaded brass shell round it out. It lobs shells far down the lane (+Y).

STATIC prop (no rig / no animation), like make_torch.py.

ENGINE FACTS (see docs/blender-model-scripting.md):
  * No GPU skinning. Each glTF primitive == one (mesh, material) pair, drawn rigidly at
    placement * partNodeWorld. So ALL same-material geometry joined into ONE object is
    ONE draw call. We keep this to 4 prims: wood / iron / bore(near-black) / brass.
  * Build Z-up in Blender; the exporter converts to glTF Y-up.

ORIENTATION / SCALE (matches the engine tile grid):
  * Footprint fits ONE 2.0 x 2.0 tile; built ~1.2 wide (wheels), well under 1.7.
  * Origin at the CENTER of the footprint, at GROUND level (base at z=0, rises in +Z).
  * Barrel points UP and toward +Y (the firing direction / lane), at a steep 60deg
    elevation from horizontal (30deg off vertical). Muzzle tip ~ (0, 0.625, 1.86).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_mortar.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

WOOD  = (0.30, 0.18, 0.09, 1.0)   # warm timber carriage
IRON  = (0.13, 0.13, 0.15, 1.0)   # dark iron barrel / bands / rims
BORE  = (0.02, 0.02, 0.025, 1.0)  # near-black hollow muzzle bore
BRASS = (0.62, 0.45, 0.16, 1.0)   # brass trunnion caps / loaded shell accent

# --- Barrel geometry (so muzzle tip is easy to recompute) ---
ELEV   = 60.0                                   # elevation from horizontal (deg)
TILT   = ELEV - 90.0                            # off vertical; rot about X tilts top toward +Y
PIVOT  = Vector((0.0, 0.0, 0.78))               # trunnion pivot between the cheek plates
DIR    = Vector((0.0, math.cos(math.radians(ELEV)), math.sin(math.radians(ELEV))))  # axis up+Y
BLEN   = 1.5                                     # barrel length (depth along its axis)
FWD    = 0.5                                     # how far the barrel center sits forward of pivot
BCEN   = PIVOT + DIR * FWD                       # barrel center
MUZZLE = BCEN + DIR * (BLEN * 0.5 + 0.10)        # muzzle tip (a hair past the cap)


def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass


def mat(name, rgba, emissive=False, estr=2.0):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    if b:
        b.inputs["Base Color"].default_value = rgba
        if "Roughness" in b.inputs: b.inputs["Roughness"].default_value = 0.75
        if "Metallic" in b.inputs and name in ("iron", "brass"):
            b.inputs["Metallic"].default_value = 0.7
        if emissive and "Emission Color" in b.inputs:
            b.inputs["Emission Color"].default_value = rgba
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = estr
    return m


def make_empty(name, parent, loc):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = 'ARROWS'; e.empty_display_size = 0.1
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


def cyl(name, r_bot, r_top, depth, loc, mat_, rot=None, segs=20):
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


def along(t):
    """A point on the barrel axis, t = signed distance from the barrel center."""
    return tuple(BCEN + DIR * t)


def main():
    reset_scene()
    wood  = mat("wood", WOOD)
    iron  = mat("iron", IRON)
    bore  = mat("bore", BORE)
    brass = mat("brass", BRASS)

    root = make_empty("mortar", None, (0, 0, 0))
    rotX = (TILT, 0, 0)   # tilt a Z-built cylinder to the barrel elevation (top toward +Y)

    # ---------------- WOOD: heavy timber carriage bed, cheek plates, wheel disks ----------------
    wood_parts = [
        # heavy beveled timber bed (the carriage block)
        box("bed",     (0.86, 1.00, 0.46), (0, -0.04, 0.27), wood, bev=0.05),
        box("bedstep", (0.74, 0.74, 0.22), (0, 0.10, 0.56),  wood, bev=0.04),  # raised step the plates sit on
        # two cheek plates the barrel pivots between
        box("cheekL",  (0.11, 0.62, 0.78), (0.34, 0.06, 0.62), wood, rot=(8, 0, 0), bev=0.03),
        box("cheekR",  (0.11, 0.62, 0.78), (-0.34, 0.06, 0.62), wood, rot=(8, 0, 0), bev=0.03),
        # rear stock / trail behind the breech for the recoil to drive into
        box("trail",   (0.30, 0.46, 0.20), (0, -0.56, 0.18), wood, rot=(-14, 0, 0), bev=0.03),
    ]
    # two small wheel disks (wood), axles along X
    for sx in (1, -1):
        wood_parts.append(cyl("wheel%d" % sx, 0.32, 0.32, 0.10,
                              (sx * 0.50, -0.12, 0.32), wood, rot=(0, 90, 0), segs=20))
    assemble("wood", root, wood_parts)

    # ---------------- IRON: the barrel, reinforcing bands, breech, wheel rims/hubs ----------------
    iron_parts = [
        # the stout tapered tube (fat breech -> slightly narrower muzzle)
        cyl("barrel", 0.27, 0.225, BLEN, tuple(BCEN), iron, rot=rotX, segs=24),
        # rounded breech cap closing the bottom of the tube
        sphere("breech", 0.27, along(-BLEN * 0.5 + 0.02), iron, scale=(1, 1, 0.8), subdiv=2),
        sphere("knob",   0.10, along(-BLEN * 0.5 - 0.10), iron, subdiv=2),  # cascabel knob
        # muzzle reinforcing rim (the swell at the mouth)
        cyl("muzzleband", 0.245, 0.255, 0.11, along(BLEN * 0.5 - 0.08), iron, rot=rotX, segs=24),
    ]
    # two iron reinforcing bands along the barrel
    for t in (0.30, -0.05):
        iron_parts.append(cyl("band", 0.285, 0.285, 0.07, along(t), iron, rot=rotX, segs=24))
    # wheel rims + hubs (iron), axles along X
    for sx in (1, -1):
        iron_parts.append(cyl("rim%d" % sx, 0.345, 0.345, 0.06,
                              (sx * 0.50, -0.12, 0.32), iron, rot=(0, 90, 0), segs=24))
        iron_parts.append(cyl("hub%d" % sx, 0.09, 0.09, 0.16,
                              (sx * 0.50, -0.12, 0.32), iron, rot=(0, 90, 0), segs=12))
        iron_parts.append(sphere("axlecap%d" % sx, 0.06, (sx * 0.56, -0.12, 0.32), iron, subdiv=1))
    assemble("iron", root, iron_parts)

    # ---------------- BORE: a deep, near-black HOLLOW cavity bored down into the muzzle ----------------
    assemble("bore", root, [
        cyl("muzzlehole", 0.185, 0.175, 0.55, along(BLEN * 0.5 - 0.27), bore, rot=rotX, segs=28),  # deep tube cavity
        cyl("borefloor",  0.175, 0.02,  0.10, along(BLEN * 0.5 - 0.55), bore, rot=rotX, segs=24),   # tapered dark floor
    ])

    # ---------------- BRASS: trunnion pivot caps + a loaded shell on the bed ----------------
    brass_parts = [
        cyl("trunL", 0.085, 0.085, 0.07, (0.40, PIVOT.y, PIVOT.z), brass, rot=(0, 90, 0), segs=14),
        cyl("trunR", 0.085, 0.085, 0.07, (-0.40, PIVOT.y, PIVOT.z), brass, rot=(0, 90, 0), segs=14),
        # a small loaded shell resting on the bed step beside the breech
        sphere("shell",   0.13, (0.30, -0.18, 0.78), brass, scale=(1, 1, 1.05), subdiv=2),
        cyl("shellfuse",  0.03, 0.025, 0.07, (0.30, -0.18, 0.95), brass, segs=10),
    ]
    assemble("brass", root, brass_parts)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "mortar.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=False, export_apply=False, use_selection=False)
    print("[make_mortar] wrote", out_path)
    print("[make_mortar] muzzle tip (model units):", tuple(round(v, 3) for v in MUZZLE))


if __name__ == "__main__":
    main()
