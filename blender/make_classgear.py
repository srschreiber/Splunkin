"""
Build the player CLASS GEAR and export three socket props:
  assets/models/knighthelm.glb  - knight helmet (head socket)
  assets/models/wizardhat.glb    - wizard hat   (head socket)
  assets/models/staff.glb        - wizard staff (hand socket, like sword.glb)

These are static single-model props drawn at a bone's world matrix (head / hand), the same
way helmet.glb and sword.glb are. No rig/animation. Z-up; built at human scale around the
ORIGIN (the bone), since the head/hand world matrix already places + scales them. If a prop
sits too high/low in-game, nudge the small offset constant where it's drawn in main.cpp.

GRAPHICS: rounded, smooth-shaded primitives (icospheres / tapered cones) with beveled hard
edges, in the spirit of make_skeleton.py -- a domed helm, a soft drooping wizard cone, a
turned wooden staff with a faceted crystal. Detail (rivets, crest, turned rings, grip wrap,
finials) is added in the SAME material as the part it sits on, then everything is JOINED into
one mesh per prop -- so each distinct MATERIAL collapses to exactly one glTF primitive / one
draw call regardless of vertex count. Kept faithful to the old scale / origin / facing (+Y).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_classgear.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector


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
        if "Roughness" in b.inputs: b.inputs["Roughness"].default_value = 0.45
        if emissive and "Emission Color" in b.inputs:
            b.inputs["Emission Color"].default_value = rgba
            if "Emission Strength" in b.inputs: b.inputs["Emission Strength"].default_value = 4.0
    return m


def _obj(name, mesh, loc, rot, mat_, scale, smooth):
    for p in mesh.polygons: p.use_smooth = smooth
    o = bpy.data.objects.new(name, mesh); bpy.context.collection.objects.link(o)
    o.location = Vector(loc); o.scale = Vector(scale)
    if rot: o.rotation_euler = tuple(math.radians(a) for a in rot)
    o.data.materials.append(mat_)
    return o


def sphere(name, r, loc, mat_, scale=(1, 1, 1), subdiv=2, rot=None, smooth=True):
    bm = bmesh.new(); bmesh.ops.create_icosphere(bm, subdivisions=subdiv, radius=r)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, scale, smooth)


def cyl(name, r_bot, r_top, depth, loc, mat_, rot=None, segs=16, scale=(1, 1, 1), smooth=True):
    """Tapered cone/cylinder along local Z (r_top==r_bot -> straight cylinder / disc)."""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs,
                          radius1=r_bot, radius2=r_top, depth=depth)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, scale, smooth)


def box(name, size, loc, mat_, rot=None, bev=0.012, smooth=False):
    bm = bmesh.new(); bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    if bev > 0:
        bmesh.ops.bevel(bm, geom=list(bm.verts) + list(bm.edges) + list(bm.faces),
                        offset=bev, segments=2, affect='EDGES')
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), smooth)


def basis(colX, colY, colZ, trans):
    """A rigid Blender 4x4 whose columns say where canonical +X/+Y/+Z map to, plus origin.
    Used to drop a sword/shield built in a clean canonical frame back into the exact
    placement frame the OLD rigged sword.glb / shield.glb exported (so it still sits in the
    hand, since the engine draws these props at hand/head_world * each part's node-local TRS,
    and join_all bakes the geometry so that TRS is identity == this placement frame)."""
    return Matrix((
        (colX[0], colY[0], colZ[0], trans[0]),
        (colX[1], colY[1], colZ[1], trans[1]),
        (colX[2], colY[2], colZ[2], trans[2]),
        (0.0, 0.0, 0.0, 1.0)))


def reframe(objs, M):
    """Premultiply every piece by M, mapping the whole assembly from its canonical build
    frame into the target placement frame before join_all bakes the transforms."""
    for o in objs:
        o.matrix_basis = M @ o.matrix_basis


def join_all(name):
    """Apply transforms and join every mesh object into one. Distinct materials survive as
    separate primitives, so PRIMS == number of materials actually used."""
    objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    bpy.ops.object.select_all(action='DESELECT')
    for o in objs: o.select_set(True)
    bpy.context.view_layer.objects.active = objs[0]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    if len(objs) > 1: bpy.ops.object.join()
    res = bpy.context.view_layer.objects.active
    res.name = name
    return res


def export(path):
    out = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", path))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_apply=False, use_selection=False)
    print("[make_classgear] wrote", out)


def knight_helm():
    """Domed barbute-style helm enclosing the head (origin ~ head center, faces +Y).
    Steel shell + dark visor recess + a swept red horsehair crest."""
    reset_scene()
    steel = mat("k_steel", (0.66, 0.69, 0.75, 1.0))
    dark  = mat("k_dark",  (0.16, 0.17, 0.20, 1.0))
    gold  = mat("k_gold",  (0.82, 0.66, 0.22, 1.0))
    plume = mat("k_plume", (0.80, 0.13, 0.13, 1.0))

    # --- STEEL: domed cranium + lower face guard, rim, nasal bar, riveted cheeks ---
    sphere("dome",   0.185, (0, -0.01, 0.075), steel, scale=(1.06, 1.10, 1.16), subdiv=3)
    sphere("guard",  0.155, (0,  0.015, -0.055), steel, scale=(0.98, 0.92, 0.92), subdiv=3)
    cyl("rim", 0.205, 0.205, 0.045, (0, 0, 0.12), steel, segs=28, scale=(1.02, 1.06, 1.0))
    box("nasal", (0.05, 0.07, 0.30), (0, 0.185, 0.02), steel, bev=0.02, smooth=True)
    sphere("cheekL", 0.075, (0.155, 0.085, -0.04), steel, scale=(0.7, 1.0, 1.05), subdiv=2)
    sphere("cheekR", 0.075, (-0.155, 0.085, -0.04), steel, scale=(0.7, 1.0, 1.05), subdiv=2)

    # --- DARK: visor eye slit + brow shadow recessed into the steel ---
    box("brow", (0.34, 0.05, 0.055), (0, 0.20, 0.135), dark, bev=0.02, smooth=True)
    box("slit", (0.30, 0.05, 0.045), (0, 0.205, 0.065), dark, bev=0.015, smooth=True)
    sphere("breathL", 0.02, (0.06, 0.215, -0.08), dark, subdiv=1)
    sphere("breathR", 0.02, (-0.06, 0.215, -0.08), dark, subdiv=1)

    # --- GOLD: brow trim band + a row of rivets along the rim ---
    box("trim", (0.30, 0.03, 0.025), (0, 0.215, 0.175), gold, bev=0.008, smooth=True)
    for i, ang in enumerate(range(-60, 61, 30)):
        a = math.radians(ang)
        sphere("rivet%d" % i, 0.016, (0.20 * math.sin(a), 0.20 * math.cos(a), 0.115),
               gold, subdiv=1)

    # --- PLUME: swept-back red crest (a fin of flattened spheres) ---
    crest = [(0, 0.05, 0.275), (0, -0.01, 0.30), (0, -0.07, 0.295),
             (0, -0.13, 0.255), (0, -0.18, 0.185), (0, -0.21, 0.10), (0, -0.215, 0.02)]
    for i, (x, y, z) in enumerate(crest):
        sphere("plume%d" % i, 0.055 - i * 0.003, (x, y, z), plume,
               scale=(0.42, 1.05, 1.15), subdiv=2)

    join_all("knighthelm")
    export("knighthelm.glb")


def wizard_hat():
    """Wide soft brim + a smooth tapering cone that droops forward to a tip, dark band with a
    gold buckle and a faceted gold star. Origin ~ head center, faces +Y."""
    reset_scene()
    blue = mat("w_hat",  (0.17, 0.21, 0.64, 1.0))
    trim = mat("w_trim", (0.09, 0.11, 0.40, 1.0))
    star = mat("w_star", (0.98, 0.84, 0.26, 1.0), emissive=True)

    # --- BLUE: domed brim (oblate sphere) + stacked cone frustums curving forward (droop) ---
    sphere("brim", 0.30, (0, 0.0, 0.155), blue, scale=(1.0, 1.0, 0.16), subdiv=3)
    cyl("cone0", 0.205, 0.155, 0.16, (0, 0.00, 0.27), blue, segs=24)
    cyl("cone1", 0.155, 0.105, 0.16, (0, 0.035, 0.42), blue, segs=24, rot=(12, 0, 0))
    cyl("cone2", 0.105, 0.055, 0.16, (0, 0.095, 0.55), blue, segs=20, rot=(26, 0, 0))
    cyl("tip",   0.055, 0.006, 0.17, (0, 0.18, 0.66), blue, segs=16, rot=(42, 0, 0))

    # --- TRIM: hatband ring around the base of the cone ---
    cyl("band", 0.212, 0.212, 0.055, (0, 0.0, 0.205), trim, segs=24, scale=(1.0, 1.0, 1.0))

    # --- STAR: faceted gold star on the band + a buckle ---
    sphere("star", 0.055, (0, 0.205, 0.225), star, scale=(1.0, 0.45, 1.0), subdiv=1, smooth=False)
    for k in range(5):
        a = math.radians(90 + k * 72)
        sphere("spike%d" % k, 0.028, (0.055 * math.cos(a), 0.205, 0.225 + 0.055 * math.sin(a)),
               star, scale=(1.0, 0.45, 1.0), subdiv=1, smooth=False)
    box("buckle", (0.05, 0.03, 0.05), (0, 0.215, 0.205), star, bev=0.01, smooth=True)

    join_all("wizardhat")
    export("wizardhat.glb")


def staff():
    """Turned wooden staff along the hand's local Z with a leather grip wrap, a metal ferrule
    and a faceted glowing crystal cradled in metal claws. Matches sword.glb's rough scale."""
    reset_scene()
    wood    = mat("s_wood",    (0.42, 0.27, 0.13, 1.0))
    leather = mat("s_leather", (0.20, 0.13, 0.07, 1.0))
    metal   = mat("s_metal",   (0.70, 0.72, 0.78, 1.0))
    gem     = mat("s_gem",     (0.40, 0.78, 1.0, 1.0), emissive=True)

    # --- WOOD: gently tapered shaft + turned rings (collar swells) along its length ---
    cyl("shaft", 0.038, 0.030, 1.46, (0, 0, 0.0), wood, segs=18)
    for z in (-0.62, -0.30, 0.10, 0.55):
        cyl("ring%.2f" % z, 0.046, 0.046, 0.03, (0, 0, z), wood, segs=18, scale=(1.0, 1.0, 1.0))

    # --- LEATHER: grip wrap (a stack of raised rings) where the hand holds it ---
    for i in range(6):
        z = -0.52 + i * 0.052
        cyl("wrap%d" % i, 0.050, 0.050, 0.036, (0, 0, z), leather, segs=16, scale=(1.0, 1.0, 1.0))

    # --- METAL: bottom ferrule (knob + spike) and the collar under the crystal ---
    sphere("knob", 0.05, (0, 0, -0.74), metal, subdiv=2)
    cyl("ferrule", 0.05, 0.012, 0.12, (0, 0, -0.83), metal, segs=14)
    cyl("collar", 0.055, 0.045, 0.07, (0, 0, 0.70), metal, segs=16)
    # claws cradling the crystal: prongs splayed out then curling up
    for k in range(4):
        a = math.radians(k * 90)
        cyl("claw%d" % k, 0.020, 0.006, 0.20, (0.07 * math.cos(a), 0.07 * math.sin(a), 0.80),
            metal, segs=8, rot=(-28 * math.sin(a), 28 * math.cos(a), 0))

    # --- GEM: faceted glowing crystal at the head ---
    sphere("crystal", 0.10, (0, 0, 0.86), gem, scale=(1.0, 1.0, 1.35), subdiv=1, smooth=False)

    join_all("staff")
    export("staff.glb")


def sword():
    """Knight's sword, drawn at the hand bone (and as the spinning orbit/thrown blades).
    Built canonically (grip at origin, blade up +Z, width along X, flat faces along Y) then
    reframed into the OLD sword.glb's exact placement frame: grip/crossguard at the hand,
    blade angled up-and-forward, tip ~3.4 units out. Slim tapered fullered blade, beveled
    crossguard, wrapped grip, rounded pommel."""
    reset_scene()
    steel   = mat("sw_steel",   (0.74, 0.77, 0.82, 1.0))
    leather = mat("sw_leather", (0.22, 0.14, 0.08, 1.0))
    gold    = mat("sw_gold",    (0.83, 0.66, 0.24, 1.0))

    objs = []
    # --- STEEL: tapered lens-section blade (segs=8 cone flattened -> rounded edges + a soft
    # central fuller ridge), the beveled crossguard, and the pommel cap. ---
    objs.append(cyl("blade", 0.185, 0.010, 3.28, (0, 0, 1.76), steel, segs=8,
                    scale=(1.0, 0.30, 1.0), smooth=True))           # blade body, points +Z
    objs.append(cyl("ricasso", 0.07, 0.06, 0.16, (0, 0, 0.10), steel, segs=12))  # blade root collar
    objs.append(box("guard", (0.86, 0.20, 0.17), (0, 0, 0.0), steel, bev=0.05, smooth=True))
    objs.append(sphere("pommel", 0.115, (0, 0, -0.78), steel, subdiv=2))
    objs.append(cyl("pommel_neck", 0.055, 0.07, 0.10, (0, 0, -0.70), steel, segs=12))

    # --- GOLD: little finials on the crossguard tips + a band where blade meets guard ---
    objs.append(sphere("tipL", 0.075, (0.43, 0, 0.0), gold, subdiv=2))
    objs.append(sphere("tipR", 0.075, (-0.43, 0, 0.0), gold, subdiv=2))
    objs.append(cyl("collar", 0.075, 0.075, 0.05, (0, 0, 0.085), gold, segs=14))

    # --- LEATHER: tapered grip core + a stack of wrap rings the hand holds ---
    objs.append(cyl("grip", 0.065, 0.058, 0.62, (0, 0, -0.40), leather, segs=14))
    for i in range(7):
        z = -0.66 + i * 0.085
        objs.append(cyl("wrap%d" % i, 0.070, 0.070, 0.05, (0, 0, z), leather, segs=14))

    reframe(objs, basis((0.993, -0.120, -0.010),   # +X (blade width)  -> old width axis
                        (0.036,  0.379, -0.925),   # +Y (flat/thick)   -> old thickness axis
                        (0.115,  0.918,  0.381),   # +Z (blade length) -> old blade axis
                        (0.046,  0.406,  0.743)))   # grip/crossguard origin
    join_all("sword")
    export("sword.glb")


def shield():
    """Knight's kite/round shield, drawn at the (off) hand bone. Built canonically as a gently
    domed disc in the XY plane (normal +Z) then reframed into OLD shield.glb's placement frame
    so it still rides the hand at the same size/tilt. Domed face, beveled rim, central boss,
    ring of rivets -- not a flat slab."""
    reset_scene()
    steel = mat("sh_steel", (0.70, 0.73, 0.79, 1.0))
    trim  = mat("sh_trim",  (0.30, 0.22, 0.10, 1.0))
    boss  = mat("sh_boss",  (0.82, 0.66, 0.24, 1.0))

    objs = []
    # --- TRIM: thick beveled rim disc (the backing/edge), slightly oval ---
    objs.append(cyl("rim", 1.78, 1.78, 0.22, (0, 0, -0.02), trim, segs=36, scale=(0.96, 1.06, 1.0)))
    # --- STEEL: domed face (oblate sphere, only the front bulge reads) ---
    objs.append(sphere("face", 1.0, (0, 0, 0.16), steel, scale=(1.60, 1.74, 0.46), subdiv=3))
    # --- BOSS: raised central boss + collar ---
    objs.append(sphere("boss", 0.34, (0, 0, 0.40), boss, scale=(1.0, 1.0, 0.85), subdiv=3))
    objs.append(cyl("boss_ring", 0.42, 0.42, 0.08, (0, 0, 0.30), boss, segs=24))
    # --- BOSS rivets around the rim ---
    for k in range(10):
        a = math.radians(k * 36)
        objs.append(sphere("rivet%d" % k, 0.07, (1.5 * math.cos(a), 1.62 * math.sin(a), 0.16),
                           boss, subdiv=2))

    reframe(objs, basis((-0.990, -0.144,  0.007),   # +X (width)        -> old width axis
                        (-0.140,  0.972,  0.191),   # +Y (height)       -> old height axis
                        (-0.035,  0.188, -0.982),   # +Z (face normal)  -> old face normal
                        (0.060,   0.014,  0.481)))   # shield center
    join_all("shield")
    export("shield.glb")


def helmet():
    """Fallback-rig helmet, drawn at the head bone enclosing the head. Built directly in the
    placement frame (head-centred at Z=1.625, faces +Y, radius ~0.9 like OLD helmet.glb).
    Smooth domed shell + beveled brow rim, dark visor slit, nasal bar and a swept crest."""
    reset_scene()
    HC = 1.625   # head centre along Z (matches old helmet.glb)
    steel = mat("hm_steel", (0.70, 0.73, 0.79, 1.0))
    dark  = mat("hm_dark",  (0.15, 0.16, 0.19, 1.0))
    plume = mat("hm_plume", (0.80, 0.13, 0.13, 1.0))

    objs = []
    # --- STEEL: domed cranium + lower face guard + brow rim + nasal bar ---
    objs.append(sphere("dome",  0.86, (0, -0.05, HC + 0.08), steel, scale=(1.0, 1.04, 1.06), subdiv=3))
    objs.append(sphere("guard", 0.74, (0,  0.06, HC - 0.34), steel, scale=(0.96, 0.92, 0.90), subdiv=3))
    objs.append(cyl("rim", 0.92, 0.92, 0.18, (0, 0, HC + 0.34), steel, segs=32, scale=(1.02, 1.04, 1.0)))
    objs.append(box("nasal", (0.18, 0.30, 0.95), (0, 0.84, HC - 0.05), steel, bev=0.06, smooth=True))

    # --- DARK: visor eye slit + brow shadow recessed into the steel ---
    objs.append(box("slit", (1.30, 0.20, 0.18), (0, 0.86, HC + 0.18), dark, bev=0.05, smooth=True))
    objs.append(box("brow", (1.40, 0.18, 0.22), (0, 0.80, HC + 0.42), dark, bev=0.06, smooth=True))

    # --- PLUME: swept-back red crest (a fin of flattened spheres along the top, front->back) ---
    crest = [(0.20, HC + 0.92), (-0.05, HC + 1.02), (-0.34, HC + 1.00),
             (-0.62, HC + 0.86), (-0.86, HC + 0.62), (-1.00, HC + 0.30)]
    for i, (y, z) in enumerate(crest):
        objs.append(sphere("plume%d" % i, 0.24 - i * 0.012, (0, y, z), plume,
                           scale=(0.34, 1.05, 1.10), subdiv=2))

    join_all("helmet")
    export("helmet.glb")


def main():
    knight_helm()
    wizard_hat()
    staff()
    sword()
    shield()
    helmet()


if __name__ == "__main__":
    main()
