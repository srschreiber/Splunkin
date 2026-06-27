"""
Procedurally build a wooden TORCH prop and export assets/models/torch.glb.

This replaces a stale torch.glb. It is a STATIC prop (no rig / no animation) placed
around the base; the FLAME is a separate in-engine particle effect, so we DON'T model
fire here. We only model the torch object: a turned wooden shaft with a knobbed grip,
a dark iron collar, and a flared iron bowl/cage at the top that cradles a small lump of
charred, faintly-glowing coals (where the engine's particle flame sits).

ENGINE FACTS (see docs/blender-model-scripting.md):
  * No GPU skinning. Each glTF primitive == one (mesh, material) pair, drawn rigidly at
    placement * partNodeWorld. So grouping all same-material geometry into ONE join keeps
    this to ~3 draw calls (wood / iron / ember).
  * Build Z-up in Blender; the exporter converts to glTF Y-up. The torch stands along +Z.
  * main.cpp (the torch loader) derives the flame anchor as the CENTROID of the model's
    EMISSIVE parts, and sits free-standing torches on their lowest point. So we MUST keep
    an emissive part (the coals) centered at the top -> flame point lands in the bowl.

LAYOUT MATCH (so it drops into the existing placement unchanged):
  * Origin at Blender (0,0,0); the torch occupies Z ~0.39 .. ~2.5 (glTF Y after export).
  * Coals (the only emissive parts) centroid at Z ~2.38 -> glTF Y ~2.38, matching the old
    torch's flame node (translation y=2.384). Bottom ~0.39 (old stick bottom ~0.46).

Run: /Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_torch.py
"""

import bpy, bmesh, math, os
from mathutils import Matrix, Vector

WOOD  = (0.34, 0.20, 0.10, 1.0)   # warm brown turned wood
IRON  = (0.12, 0.12, 0.14, 1.0)   # dark iron collar / bowl / cage
CHAR  = (0.16, 0.07, 0.03, 1.0)   # charred coal base (faint ember emission)


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
        if "Roughness" in b.inputs: b.inputs["Roughness"].default_value = 0.8
        if emissive and "Emission Color" in b.inputs:
            b.inputs["Emission Color"].default_value = (0.95, 0.36, 0.08, 1.0)
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


def cyl(name, r_bot, r_top, depth, loc, mat_, rot=None, segs=16):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs,
                          radius1=r_bot, radius2=r_top, depth=depth)
    m = bpy.data.meshes.new(name); bm.to_mesh(m); bm.free()
    return _obj(name, m, loc, rot, mat_, (1, 1, 1), True)


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


def main():
    reset_scene()
    wood = mat("wood", WOOD)
    iron = mat("iron", IRON)
    ember = mat("ember", CHAR, emissive=True, estr=2.2)

    root = make_empty("torch", None, (0, 0, 0))

    # --- WOOD: a turned shaft, knobbed grip, rounded butt. All one material -> one draw call.
    wood_parts = [
        # rounded butt cap at the bottom
        sphere("butt", 0.135, (0, 0, 0.50), wood, scale=(1.0, 1.0, 0.80), subdiv=2),
        # lower shaft
        cyl("shaft0", 0.125, 0.115, 0.32, (0, 0, 0.64), wood),
        # knobbed grip rings (turned bulges)
        sphere("grip0", 0.150, (0, 0, 0.86), wood, scale=(1.0, 1.0, 0.50), subdiv=2),
        cyl("shaft1", 0.115, 0.105, 0.34, (0, 0, 1.07), wood),
        sphere("grip1", 0.140, (0, 0, 1.27), wood, scale=(1.0, 1.0, 0.42), subdiv=2),
        # mid shaft
        cyl("shaft2", 0.105, 0.095, 0.42, (0, 0, 1.50), wood),
        # upper shaft up under the collar
        cyl("shaft3", 0.095, 0.090, 0.30, (0, 0, 1.86), wood),
    ]
    assemble("wood", root, wood_parts)

    # --- IRON: collar band (+ rivets), the flared bowl, and the cradle cage bars.
    iron_parts = [
        # collar / band where bowl meets shaft
        cyl("collar", 0.118, 0.118, 0.11, (0, 0, 1.97), iron),
        cyl("ring",   0.130, 0.130, 0.04, (0, 0, 2.04), iron),
        # flared bowl that holds the coals
        cyl("bowl",   0.110, 0.260, 0.34, (0, 0, 2.20), iron),
    ]
    # rivets around the collar
    for k in range(4):
        a = math.radians(45 + k * 90)
        iron_parts.append(sphere("rivet%d" % k, 0.022,
                                  (0.12 * math.cos(a), 0.12 * math.sin(a), 1.97), iron, subdiv=1))
    # vertical cage bars cradling the bowl rim (lean slightly outward to follow the flare)
    for k in range(6):
        a = math.radians(k * 60)
        r = 0.215
        iron_parts.append(cyl("bar%d" % k, 0.015, 0.013, 0.40,
                              (r * math.cos(a), r * math.sin(a), 2.30), iron,
                              rot=(-12 * math.sin(a), 12 * math.cos(a), 0), segs=8))
    assemble("iron", root, iron_parts)

    # --- EMBER: charred coals lump (the ONLY emissive part -> defines the flame anchor).
    # Centroid ~Z 2.38 so the in-engine particle flame sits in the bowl.
    coals = [
        sphere("coal0", 0.105, (0.00,  0.00, 2.36), ember, scale=(1.0, 1.0, 0.75), subdiv=2),
        sphere("coal1", 0.070, (0.07,  0.05, 2.42), ember, subdiv=1),
        sphere("coal2", 0.068, (-0.06, -0.05, 2.41), ember, subdiv=1),
        sphere("coal3", 0.060, (0.00, -0.07, 2.34), ember, subdiv=1),
        sphere("coal4", 0.058, (-0.05, 0.06, 2.35), ember, subdiv=1),
    ]
    assemble("coals", root, coals)

    out_path = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "assets", "models", "torch.glb"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.export_scene.gltf(filepath=out_path, export_format='GLB',
                              export_animations=False, export_apply=False, use_selection=False)
    print("[make_torch] wrote", out_path)


if __name__ == "__main__":
    main()
