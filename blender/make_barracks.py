"""
Procedurally build a medieval timber BARRACKS building and export assets/models/barracks.glb.

This is a STATIC player-base structure that musters lane troops (see the MOBA lane pivot).
It replaces a placeholder that was just two flat tinted boxes. We model a handsome low-poly
Valheim-ish timber barracks hut: timber-framed plaster walls, a steep GABLED (peaked) roof
with a ridge beam, a recessed plank DOOR with a little awning on the front, a shuttered
window, corner posts/braces, and a tall BANNER POLE with a small FLAG.

ENGINE FACTS (see docs/blender-model-scripting.md):
  * No GPU skinning. Each glTF primitive == one (mesh, material) pair, drawn rigidly at
    placement * partNodeWorld. So grouping all same-material geometry into ONE join keeps
    the whole building to ~5 draw calls.
  * Build Z-up in Blender; the exporter converts to glTF Y-up. The building rises along +Z.
  * Static prop: a single root empty, no rig / no animation (template: make_torch.py).

ORIENTATION / SCALE (matches the engine grid so it drops onto one map tile):
  * Footprint ONE tile = 2.0 x 2.0 world units; we fill ~1.9 x 1.9 (small margin on the tile).
  * Origin at the CENTER of the footprint, at GROUND level (z=0 base, rises in +Z).
  * The DOOR faces +Y (the engine applies the piece's yaw). The roof ridge runs along X, so
    the +Y front shows a triangular gable above the door.
  * Total height ~2.0 units (ridge), banner pole pokes a bit higher; within the 1.8-2.2 band
    for the building proper.

The FLAG is its own mesh/material ("banner") so it stays identifiable for per-team tinting.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_barracks.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

WOOD    = (0.30, 0.17, 0.08, 1.0)   # warm timber frame / posts / beams / pole
PLASTER = (0.80, 0.74, 0.60, 1.0)   # daub / plaster infill panels + gables
ROOF    = (0.36, 0.22, 0.13, 1.0)   # wood-shingle / thatch roof planes
DARK    = (0.12, 0.08, 0.05, 1.0)   # recessed plank door + window pane
BANNER  = (0.62, 0.10, 0.12, 1.0)   # team flag (recolored per team in-engine)

HALF     = 0.84    # wall plane offset from center  (plaster core ~1.68 wide)
WALL_TOP = 1.12    # top of the walls
RIDGE_Z  = 2.00    # roof ridge height (building top)
EAVE_Y   = 0.94    # eave overhang in +/-Y
EAVE_Z   = 0.98    # eave height where the roof meets the wall line


def reset_scene():
    bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete()
    for blk in (bpy.data.meshes, bpy.data.objects, bpy.data.actions, bpy.data.materials):
        for d in list(blk):
            try: blk.remove(d)
            except Exception: pass


def mat(name, rgba):
    m = bpy.data.materials.new(name); m.use_nodes = True
    b = m.node_tree.nodes.get("Principled BSDF")
    if b:
        b.inputs["Base Color"].default_value = rgba
        if "Roughness" in b.inputs: b.inputs["Roughness"].default_value = 0.85
    return m


def make_empty(name, parent, loc):
    e = bpy.data.objects.new(name, None)
    e.empty_display_type = 'ARROWS'; e.empty_display_size = 0.2
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


def box(name, size, loc, mat_, rot=None, bev=0.01):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), False)


def cyl(name, r_bot, r_top, depth, loc, mat_, rot=None, segs=12):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs,
                          radius1=r_bot, radius2=r_top, depth=depth)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), True)


def sphere(name, r, loc, mat_, scale=(1, 1, 1), subdiv=2):
    bm = bmesh.new(); bmesh.ops.create_icosphere(bm, subdivisions=subdiv, radius=r)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, None, mat_, scale, True)


def gable(name, hx, z0, z1, thick, y, mat_):
    """Triangular gable wall: base width 2*hx at height z0, apex at (x=0, z1),
    standing in the X-Z plane at the given Y, with thickness `thick` along Y."""
    bm = bmesh.new()
    pts = [(-hx, z0), (hx, z0), (0.0, z1)]   # (x, z)
    back  = [bm.verts.new((x, y - thick / 2, z)) for (x, z) in pts]
    front = [bm.verts.new((x, y + thick / 2, z)) for (x, z) in pts]
    bm.faces.new(back)
    bm.faces.new(list(reversed(front)))
    for i in range(3):
        j = (i + 1) % 3
        bm.faces.new([back[i], back[j], front[j], front[i]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, (0, 0, 0), None, mat_, (1, 1, 1), False)


def assemble(name, root, objs):
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs: o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    if len(objs) > 1: bpy.ops.object.join()
    res = bpy.context.view_layer.objects.active
    res.name = name
    res.parent = root; res.matrix_parent_inverse = Matrix.Identity(4)
    res.location = Vector((0, 0, 0))
    return res


def main():
    reset_scene()
    wood    = mat("wood", WOOD)
    plaster = mat("plaster", PLASTER)
    roof    = mat("roof", ROOF)
    dark    = mat("dark", DARK)
    banner  = mat("banner", BANNER)

    root = make_empty("barracks", None, (0, 0, 0))

    roof_ang = math.degrees(math.atan2(RIDGE_Z - EAVE_Z, EAVE_Y))  # slope of the roof planes
    cy = EAVE_Y / 2.0                       # roof-plane centre in Y
    cz = (RIDGE_Z + EAVE_Z) / 2.0           # roof-plane centre in Z

    # ---------- PLASTER: wall infill core + the two gable end triangles ----------
    plaster_parts = [
        box("walls", (2 * HALF, 2 * HALF, WALL_TOP), (0, 0, WALL_TOP / 2), plaster),
        gable("gableY+", HALF, WALL_TOP - 0.02, RIDGE_Z, 0.10,  HALF, plaster),
        gable("gableY-", HALF, WALL_TOP - 0.02, RIDGE_Z, 0.10, -HALF, plaster),
    ]
    assemble("plaster", root, plaster_parts)

    # ---------- WOOD: corner posts, sill/rail/plate beams, braces, frames, awning, pole ----------
    wood_parts = []
    PO = HALF + 0.02   # posts proud of the plaster wall
    for sx in (-1, 1):
        for sy in (-1, 1):
            wood_parts.append(box("post", (0.16, 0.16, WALL_TOP + 0.04),
                                   (sx * (HALF - 0.04), sy * (HALF - 0.04), WALL_TOP / 2), wood))
    # horizontal beams: sill (low), mid rail, top plate -- on all four sides
    for z in (0.07, 0.56, WALL_TOP - 0.02):
        wood_parts.append(box("beamY+", (2 * PO, 0.12, 0.12), (0,  PO, z), wood))
        wood_parts.append(box("beamY-", (2 * PO, 0.12, 0.12), (0, -PO, z), wood))
        wood_parts.append(box("beamX+", (0.12, 2 * PO, 0.12), ( PO, 0, z), wood))
        wood_parts.append(box("beamX-", (0.12, 2 * PO, 0.12), (-PO, 0, z), wood))
    # diagonal braces on the side (+/-X) walls, lower panel, for the timber-frame look
    for sx in (-1, 1):
        for sy in (-1, 1):
            wood_parts.append(box("brace", (0.10, 0.10, 0.62),
                                   (sx * PO, sy * 0.42, 0.34), wood,
                                   rot=(sy * 36, 0, 0)))
    # door frame on +Y face (jambs + lintel) -- reads as a recessed opening
    wood_parts.append(box("jambL", (0.09, 0.10, 0.86), (-0.27, PO, 0.43), wood))
    wood_parts.append(box("jambR", (0.09, 0.10, 0.86), ( 0.27, PO, 0.43), wood))
    wood_parts.append(box("lintel", (0.70, 0.10, 0.12), (0, PO, 0.84), wood))
    # window frame + mullions on the +X side wall
    wood_parts.append(box("winframe", (0.10, 0.50, 0.50), (PO, 0.22, 0.66), wood))
    wood_parts.append(box("winmulV", (0.12, 0.05, 0.46), (PO + 0.005, 0.22, 0.66), wood))
    wood_parts.append(box("winmulH", (0.12, 0.46, 0.05), (PO + 0.005, 0.22, 0.66), wood))
    # awning over the door: a small sloped shingle-bracketed roof on wood brackets
    wood_parts.append(box("awning", (0.90, 0.40, 0.05), (0, PO + 0.16, 0.99), wood,
                          rot=(-32, 0, 0)))
    for sx in (-1, 1):
        wood_parts.append(box("bracket", (0.07, 0.30, 0.07), (sx * 0.34, PO + 0.12, 0.92), wood,
                              rot=(38, 0, 0)))
    # ridge beam capping the roof peak
    wood_parts.append(box("ridge", (1.86, 0.13, 0.13), (0, 0, RIDGE_Z), wood))
    # banner pole at the back +X/-Y corner, rising above the roof, with a finial knob
    PX, PY = HALF - 0.06, -(HALF - 0.06)
    wood_parts.append(cyl("pole", 0.05, 0.04, 2.74, (PX, PY, 1.37), wood, segs=8))
    wood_parts.append(sphere("finial", 0.075, (PX, PY, 2.78), wood, subdiv=1))
    assemble("wood", root, wood_parts)

    # ---------- ROOF: two steep sloped planes meeting at the ridge ----------
    roof_parts = [
        box("roofY+", (1.88, 1.42, 0.09), (0,  cy, cz), roof, rot=(-roof_ang, 0, 0)),
        box("roofY-", (1.88, 1.42, 0.09), (0, -cy, cz), roof, rot=( roof_ang, 0, 0)),
    ]
    assemble("roof", root, roof_parts)

    # ---------- DARK: recessed plank door (+Y) and the window pane (+X) ----------
    dark_parts = [
        box("door", (0.48, 0.10, 0.80), (0, HALF - 0.04, 0.41), dark),
        box("pane", (0.06, 0.40, 0.40), (HALF - 0.03, 0.22, 0.66), dark),
    ]
    assemble("dark", root, dark_parts)

    # ---------- BANNER: the team FLAG (separate mesh/material for per-team tinting) ----------
    flag = box("banner", (0.03, 0.52, 0.34), (PX, PY + 0.30, 2.46), banner)
    assemble("banner", root, [flag])

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "barracks.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=False, export_apply=False, use_selection=False)
    print("[make_barracks] wrote", out_path)


if __name__ == "__main__":
    main()
