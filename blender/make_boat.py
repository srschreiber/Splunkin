"""
Procedurally build a wooden BOAT hull and export assets/models/boat.glb.

Replaces the crude procedural box boats with one clean wooden hull that reads well as a
small ROWBOAT at scale 1 and (scaled up) as a bigger warship. A skeleton sits in the
rowboat and rows, so the two OARS are on their own bones and sweep in the "walk" clip.

KEY ENGINE FACTS (see docs/blender-model-scripting.md):
  * No GPU skinning, no face culling. Each glTF *primitive* is a rigid "part" pinned to the
    NODE that holds the mesh, drawn at placement * partNodeWorld. The rig is a hierarchy of
    mesh-less EMPTIES; meshes are parented to those bones and follow them rigidly.
  * A glTF primitive == one (mesh, material) pair, so ALL same-material geometry JOINED onto
    one bone collapses into ONE primitive == ONE draw call.
  * Clips routed by name: idle / walk (others ignored). The engine plays "walk" continuously
    while the boat moves (= rowing) and "idle" while it sits still (= a gentle bob).

ORIENTATION / SCALE (must match the engine):
  * Z up in Blender (exporter converts to Y-up). BOW (front) points +Y; hull long axis is Y.
  * Origin at the WATERLINE, centered: the rim sits above z=0, the keel a bit below, so the
    model floats half-submerged at z=0.
  * Hull ~2.0 long (Y) x ~0.9 wide (X) x ~0.5 tall (Z): a rowboat at scale 1.

RIG (mesh-less empties):
  root -> oarL (+X side), oarR (-X side)
  The hull / benches / trim are static and parented to root (they bob with it in idle).
  Each oar is parented to its own bone so the two sweep independently in the row stroke.

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_boat.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

WOOD = (0.46, 0.29, 0.15, 1.0)   # warm planking brown
TRIM = (0.20, 0.12, 0.07, 1.0)   # darker gunwale / keel / posts

# Hull stations from stern (-Y) to bow (+Y): (y, half-beam X, keel_z, rim_z).
# rim_z rises at the ends (sheer); keel_z dips deepest amidships. Origin (waterline) = z 0.
STATIONS = [
    (-1.00, 0.085, -0.020, 0.300),   # stern tip (rounded, lifts out of water)
    (-0.86, 0.200, -0.100, 0.285),
    (-0.66, 0.320, -0.160, 0.265),
    (-0.42, 0.410, -0.200, 0.250),
    (-0.12, 0.450, -0.225, 0.245),   # midship: widest + deepest
    ( 0.18, 0.440, -0.215, 0.245),
    ( 0.46, 0.385, -0.185, 0.250),
    ( 0.68, 0.295, -0.140, 0.265),
    ( 0.85, 0.175, -0.080, 0.285),
    ( 1.00, 0.030,  0.010, 0.310),   # bow tip (sharp, rides up)
]
SIDE = 5   # cross-section points per side -> 2*SIDE+1 per ring


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
    e.empty_display_type = 'ARROWS'; e.empty_display_size = 0.12
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


def box(name, size, loc, mat_, rot=None, bev=0.012, smooth=False):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), smooth)


def cyl(name, r_bot, r_top, depth, loc, mat_, rot=None, segs=12):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs,
                          radius1=r_bot, radius2=r_top, depth=depth)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), True)


def bm_to_obj(name, bm, mat_, smooth=True):
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    for p in m.polygons: p.use_smooth = smooth
    o = bpy.data.objects.new(name, m); bpy.context.collection.objects.link(o)
    o.data.materials.append(mat_)
    return o


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


# ---------------------------------------------------------------- hull geometry

def section(st, inner, thick):
    """An open cross-section polyline: port rim -> down -> keel -> up -> starboard rim.
    `inner` shrinks it inward by `thick` to give the hull wall a real thickness. The bilge
    is a quarter-ellipse (sin / 1-cos) so the shape is a soft rounded V, smooth-shaded."""
    y, halfbeam, keel_z, rim_z = st
    hb = max(halfbeam - thick, 0.012) if inner else halfbeam
    kz = (keel_z + thick) if inner else keel_z
    rz = (rim_z - thick * 0.3) if inner else rim_z
    pts = []
    for k in range(SIDE, 0, -1):                 # port rim -> keel (x negative)
        s = k / SIDE
        pts.append((-hb * math.sin(s * math.pi / 2),
                    y, kz + (rz - kz) * (1 - math.cos(s * math.pi / 2))))
    pts.append((0.0, y, kz))                     # keel center
    for k in range(1, SIDE + 1):                 # keel -> starboard rim (x positive)
        s = k / SIDE
        pts.append((hb * math.sin(s * math.pi / 2),
                    y, kz + (rz - kz) * (1 - math.cos(s * math.pi / 2))))
    return pts


def build_hull(mat_, thick=0.05):
    """Double-wall wooden trough: outer skin + inner skin (the interior floor & sides),
    closed across the bow and stern. The top mouth stays open (the cockpit). Returns the
    object plus the per-station outer/inner rim coords so the trim can cap the gunwale."""
    bm = bmesh.new()
    nS, K = len(STATIONS), 2 * SIDE + 1
    outc = [section(st, False, thick) for st in STATIONS]
    inc  = [section(st, True,  thick) for st in STATIONS]
    Vout = [[bm.verts.new(p) for p in outc[i]] for i in range(nS)]
    Vin  = [[bm.verts.new(p) for p in inc[i]]  for i in range(nS)]

    def quad(a, b, c, d):
        try: bm.faces.new((a, b, c, d))
        except ValueError: pass

    for i in range(nS - 1):
        for j in range(K - 1):
            quad(Vout[i][j], Vout[i][j + 1], Vout[i + 1][j + 1], Vout[i + 1][j])   # outer skin
            quad(Vin[i][j], Vin[i + 1][j], Vin[i + 1][j + 1], Vin[i][j + 1])       # inner skin
    for j in range(K - 1):
        quad(Vout[0][j], Vin[0][j], Vin[0][j + 1], Vout[0][j + 1])                 # stern cap
        quad(Vout[nS - 1][j], Vout[nS - 1][j + 1], Vin[nS - 1][j + 1], Vin[nS - 1][j])  # bow cap
    return bm_to_obj("hull", bm, mat_), outc, inc


def build_trim(outc, inc, mat_, thick=0.05):
    """Dark gunwale rim capping the wall thickness all the way around, raised a touch into a
    proud rail; built as a separate (trim-coloured) ribbon over the hull's open top edge."""
    bm = bmesh.new()
    nS, K = len(outc), 2 * SIDE + 1
    dz = 0.014

    def rv(p):
        return bm.verts.new((p[0], p[1], p[2] + dz))

    Po = [rv(outc[i][0])      for i in range(nS)]   # port outer rim
    Pi = [rv(inc[i][0])       for i in range(nS)]   # port inner rim
    So = [rv(outc[i][K - 1])  for i in range(nS)]   # starboard outer rim
    Si = [rv(inc[i][K - 1])   for i in range(nS)]   # starboard inner rim

    def quad(a, b, c, d):
        try: bm.faces.new((a, b, c, d))
        except ValueError: pass

    for i in range(nS - 1):
        quad(Po[i], Po[i + 1], Pi[i + 1], Pi[i])            # port gunwale cap
        quad(So[i], Si[i], Si[i + 1], So[i + 1])            # starboard gunwale cap
    quad(Po[0], Pi[0], Si[0], So[0])                        # stern rim band
    quad(Po[nS - 1], So[nS - 1], Si[nS - 1], Pi[nS - 1])    # bow rim band
    return bm_to_obj("gunwale", bm, mat_)


def build_oar(side, wood):
    """One oar built in its bone's LOCAL space (pivot = the oarlock at the rim). `side` = +1
    for oarL (+X, blade outboard +X) / -1 for oarR. Loom (shaft) runs outboard along X with a
    handle inboard; a flat vertical blade hangs at the outboard tip, dipping below z=0."""
    s = side
    shaft = cyl("loom", 0.024, 0.020, 0.62, (s * 0.13, 0.0, -0.03), wood, rot=(0, 90, 0), segs=10)
    grip  = cyl("grip", 0.030, 0.030, 0.14, (s * -0.20, 0.0, -0.02), wood, rot=(0, 90, 0), segs=10)
    neck  = cyl("neck", 0.020, 0.014, 0.10, (s * 0.42, 0.0, -0.10), wood, rot=(0, 90, 0), segs=8)
    blade = box("blade", (0.030, 0.10, 0.34), (s * 0.49, 0.0, -0.22), wood, rot=(0, 8 * s, 0), bev=0.01, smooth=True)
    return [shaft, grip, neck, blade]


def main():
    reset_scene()
    wood, trim = mat("wood", WOOD), mat("trim", TRIM)

    # --- Rig: root + one bone per oar (at the gunwale, just fore of midship). ---
    root = make_empty("root", None, (0, 0, 0))
    oarL = make_empty("oarL", root, (0.42, 0.12, 0.235))
    oarR = make_empty("oarR", root, (-0.42, 0.12, 0.235))

    # --- HULL (wood) + benches/thwarts joined into ONE primitive on root. ---
    hull, outc, inc = build_hull(wood)
    benchA = box("thwartA", (0.78, 0.10, 0.05), (0, -0.18, 0.165), wood, bev=0.02)
    benchB = box("thwartB", (0.66, 0.10, 0.05), (0,  0.36, 0.175), wood, bev=0.02)
    floor  = box("riser",   (0.30, 1.10, 0.04), (0, 0.0, -0.13),  wood, bev=0.02)  # keelson/footboard
    assemble("hull", root, [hull, benchA, benchB, floor])

    # --- TRIM (dark) gunwale rim + keel strip + stem/stern posts -> ONE primitive on root. ---
    gunwale = build_trim(outc, inc, trim)
    keel   = box("keel",     (0.055, 1.05, 0.07), (0, -0.05, -0.205), trim, bev=0.02)
    stem   = box("stempost", (0.05, 0.14, 0.30),  (0, 0.95, 0.18),    trim, rot=(18, 0, 0), bev=0.02)
    stern  = box("sternpost",(0.06, 0.11, 0.26),  (0, -0.965, 0.15),  trim, rot=(-14, 0, 0), bev=0.02)
    cleatA = box("cleatA",   (0.84, 0.04, 0.04),  (0, -0.18, 0.205),  trim, bev=0.015)  # rail over thwart
    assemble("trim", root, [gunwale, keel, stem, stern, cleatA])

    # --- OARS: one per bone so they sweep independently. ---
    assemble("oarL_mesh", oarL, build_oar(+1, wood))
    assemble("oarR_mesh", oarR, build_oar(-1, wood))

    # --- Animations -----------------------------------------------------------
    # walk = ROWING: oars sweep fore<->aft (Z) with a catch / pull / lift-feather / recover
    # cycle. oarR mirrors oarL (negated) so both blades pull astern together. Looped (1..25).
    walk = bpy.data.actions.new("walk")
    #         frame, sweepZ, feather/liftX
    L = [(1, 30, 4), (8, -28, 6), (13, -26, -22), (19, 2, -20), (25, 30, 4)]
    for frame, z, x in L:
        key_rot(oarL, walk, frame, (x,  0,  z))
        key_rot(oarR, walk, frame, (x,  0, -z))

    # idle = a gentle bob/rock of the whole boat (root) + oars resting, lightly feathering.
    idle = bpy.data.actions.new("idle")
    for frame, pitch, roll, rise in [(1, 1.2, 1.6, 0.0), (30, -1.2, -1.6, 0.025), (60, 1.2, 1.6, 0.0)]:
        key_rot(root, idle, frame, (pitch, roll, 0))
        key_loc(root, idle, frame, (0, 0, rise))
    for frame, x, z in [(1, -8, 6), (30, -5, 9), (60, -8, 6)]:   # oars shipped/resting, feathered
        key_rot(oarL, idle, frame, (x, 0,  z))
        key_rot(oarR, idle, frame, (x, 0, -z))

    # Reset every animated bone to its neutral REST transform before export, else the exporter
    # bakes the last keyframe pose as the node's rest pose (boat would float permanently tilted).
    for obj in (root, oarL, oarR):
        if obj.animation_data: obj.animation_data.action = None
        obj.rotation_euler = (0, 0, 0); obj.location = obj.location if obj is not root else (0, 0, 0)
    root.location = (0, 0, 0)

    def stash(obj, act):
        if not obj.animation_data: obj.animation_data_create()
        tr = obj.animation_data.nla_tracks.new(); tr.name = act.name + obj.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    stash(root, idle)
    stash(oarL, walk); stash(oarR, walk)
    stash(oarL, idle); stash(oarR, idle)

    # --- Report bounding size (model units) over the static hull+trim ---------
    bpy.context.view_layer.update()
    def bbox(names):
        lo = [1e9] * 3; hi = [-1e9] * 3
        for o in bpy.data.objects:
            if o.type != 'MESH' or o.name not in names: continue
            for v in o.data.vertices:
                w = o.matrix_world @ v.co
                for k in range(3):
                    lo[k] = min(lo[k], w[k]); hi[k] = max(hi[k], w[k])
        return lo, hi
    lo, hi = bbox({"hull", "trim"})
    print("[make_boat] HULL  X[%.3f,%.3f] Y[%.3f,%.3f] Z[%.3f,%.3f]  L=%.3f W=%.3f H=%.3f  waterline z=0" % (
        lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], hi[1] - lo[1], hi[0] - lo[0], hi[2] - lo[2]))
    lo, hi = bbox({"hull", "trim", "oarL_mesh", "oarR_mesh"})
    print("[make_boat] +OARS X[%.3f,%.3f] (oars at rest sweep out to the sides)" % (lo[0], hi[0]))

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "boat.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=True, export_animation_mode='ACTIONS',
                              export_apply=False, use_selection=False)
    print("[make_boat] wrote", out_path)


if __name__ == "__main__":
    main()
