# Authoring models with Blender Python (bpy) for this engine

How to procedurally build a `.glb` that loads, renders, and animates correctly here.
Reference example: `blender/make_skeleton.py` → `assets/models/skeleton.glb`.

Run a script headless:

```sh
/Applications/Blender.app/Contents/MacOS/Blender --background --python blender/make_skeleton.py
# (Linux/Windows: put `blender` on PATH and use `blender --background --python ...`)
```

## The one rule that matters: rigid bone-parenting, NOT skinning

The loader (`src/engine/renderer/model.cpp` `read_model`) and `pose_model`
(`src/engine/renderer/animator.cpp`) **do not do GPU skinning**. They:

1. Read every glTF node's local TRS + parent (`out.nodes`).
2. Read each mesh primitive as a "part" assigned to **the node that holds the mesh**
   (`part.node`). Skin/joints/weights are **ignored**.
3. Animate by sampling node TRS keyframes, composing world matrices down the hierarchy,
   and drawing each part at `placement * partNodeWorld`.

**Consequence:** a single mesh skinned to an armature loads as **one static part** on the
mesh node and never animates (this was the original skeleton bug — it showed as 1 part).

**Do this instead:** build the rig as a hierarchy of nodes (Empties work great), and parent
a **separate mesh object to each bone**. Each mesh becomes its own part on a node that is a
child of (or is) the animated bone, so it follows the bone rigidly. Verify with the count:
the skeleton should load as ~10 parts, not 1.

To get clean local transforms when parenting in bpy:

```python
obj.parent = bone_empty
obj.matrix_parent_inverse = Matrix.Identity(4)   # else Blender bakes an inverse offset
obj.location = local_offset_from_bone
```

## Naming the loader keys off (must match exactly)

Bones are matched **by node name** with `find_bone` (which also requires the node has **no
mesh**, so bone nodes must be Empties / mesh-less):

| name    | role |
|---------|------|
| `body`  | torso; pitched so arms/weapon aim with the look |
| `head`  | head-look bone |
| `armL`  | the **punch** layer is masked to this bone + its subtree |
| `armR`  | the **block** layer is masked to this bone |
| `handL` / `handR` | sockets for held items (sword/shield hang here) |

Extra bones (`legL`, `legR`, feet, …) are fine — they just animate; the loader doesn't
special-case them.

Animations are routed **by clip name**: `walk`, `punch`, `block`, `open`, `close`
(others are ignored). Export with one glTF animation per action:

```python
bpy.ops.export_scene.gltf(filepath=..., export_format='GLB',
                          export_animations=True, export_animation_mode='ACTIONS')
```
Park each action on an NLA track (per object) so all of them export, not just the active one.

## Transform / orientation conventions

- **Up axis:** build Z-up in Blender; the glTF exporter converts to Y-up. Match the existing
  models so heights line up.
- **Origin = waist.** Put the model origin at the waist with **feet near Z = -1.0** and head
  near **Z = +0.8** (matches `player.glb` bounds `y[-1.0, 0.8]`). The renderer lifts models by
  `MODEL_FOOT_LIFT = 1.0` so feet rest on the ground.
- **Facing:** the renderer rotates models by `MODEL_YAW_OFFSET = -90°`. If a new model faces
  sideways/backwards in-game, either rebuild it facing the same way as `player.glb` or add a
  per-model yaw offset at its draw site (cheaper than re-exporting).
- **Straight limbs export cleanly.** Axis-aligned boxes for arms/legs (hanging straight down)
  avoid having to orient diagonal geometry; the bone rotation supplies the motion.
- **Facing axis recap:** all the char models built here face **+Y** in Blender and render
  correctly with `-yaw + MODEL_YAW_OFFSET` (no extra offset). The eye model needs an extra
  `+90°` because *it* was authored facing a different way — don't copy that +90 onto new +Y
  models (that bug made the bat sit 90° off until removed).

## Recipes that worked (skeleton / bat / gnome / mage / turret)

- **Multi-color detail without textures.** The model shader is flat per-part color; give each
  box its own material (`make_part(..., material)`) and draw the whole model with a **white
  tint** so the part colors show through. This is how the gnome (green/red/white/brown) and
  bat (purple body, red eyes, pink ears) get their colors. Tint non-white only to recolor the
  whole model (e.g. skeleton bone-white, hit-flash red, elite gold).
- **Glowing parts.** Set the Principled BSDF **Emission Color = base color, Emission Strength
  ~3–4**; the loader reads `emissiveFactor * KHR_materials_emissive_strength` into `part.emissive`
  (added unlit in the shader). Used for ember skeleton eyes, red bat eyes, the mage's staff orb.
- **Pointy hats / cones** = a stack of shrinking boxes going up (brim → wide → … → tip), with
  the tip nudged forward in +Y for a droop. See the gnome (red) and mage (tall blue) hats.
- **Held props** (staff, weapon): parent a box to a **hand bone** (`handL`/`handR`); it inherits
  the hand's animated transform. The gnome/mage staff is a tall brown box on `handR` extending up.
- **Static props (no rig/anim)** like the turret: origin at **ground (Z=0)**, not the waist;
  no special bone names needed. Draw it at terrain height (no foot lift). The turret base/housing
  is a static `.glb` drawn yawed toward the target, while the **barrel + tracer stay procedural**
  so the gun still aims/pitches in real time — a model only needs to cover the parts that don't
  move per-frame.

## Re-skinning an existing enemy (keep behavior, change look)

The gnome/mage keep the flamethrower/ranged **behavior** and only swap the rendered model. To do
this: add an `EnemyKind` only if behavior differs (skeleton/bat did; gnome/mage did NOT — they
reuse `Flamethrower`/`Ranged`). In the ground-enemy draw, pick `model/data` per kind (`is_gnome`,
`is_mage`, …) with a `*_loaded` guard, pose with that model's own `walk`/`punch`, and tint white.
To make a kind never spawn as the old look, route the spawner's fallback to the new kind (we set
the melee fallback to `Skeleton`, retiring the green melee enemy).

## Vertex layout / materials

- Each primitive is interleaved as **pos(3) · normal(3) · uv(2)**. Missing normals/UVs are
  fine (defaults filled in), so a quick box mesh with no UVs loads.
- Material `base_color_factor` is read into `part.color`, but **enemies/players are drawn with
  a flat color passed at draw time** (e.g. skeletons are tinted bone-white in `main.cpp`), so
  material color is mostly cosmetic for in-Blender preview.

## Wiring a new model into the engine

1. `read_model("assets/models/foo.glb", foo_data)` near the other loads (it's fine for this to
   run before the GL context exists — it's CPU-only).
2. **Upload AFTER `renderer.init()`**: `Model foo_model; foo_model.upload(foo_data);`. Calling
   `.upload()` (which makes GL calls) before the context exists **segfaults with an empty log**
   — this bit the skeleton on first try.
3. Render: `pose_model(foo_data, layers, …)` then `draw_model(foo_model, part_world, place, color)`.
   Reuse `foo_data.walk` / `foo_data.punch` and `foo_data.arm_l_node` for the punch mask.
4. Keep it optional: if `read_model` returns false, fall back so the game still runs without the asset.

## Gotchas checklist

- [ ] Separate mesh part per bone (not one skinned mesh). Confirm part count > 1.
- [ ] Bones are mesh-less nodes named exactly `body/head/armL/armR/handL/handR`.
- [ ] Actions named `walk`/`punch`; exported with `export_animation_mode='ACTIONS'` + NLA tracks.
- [ ] Origin at waist, feet ≈ Z -1.0, Z-up.
- [ ] `.upload()` only after the GL context exists.
- [ ] Debug-print `nodes / parts / bone indices / walk·punch valid` once at load to verify.
- [ ] **Reset every bone's `rotation_euler` to (0,0,0) AFTER authoring clips, BEFORE export.**
      Keyframing leaves each object at its *last* keyframe pose; the glTF exporter bakes that
      as the node's REST transform, so the model stands permanently tilted/posed (e.g. a
      roll clip's lean leaks into the standing pose). Engine clips are full TRS overrides, so
      a neutral rest pose + the stashed actions is what you want. New clip names also need
      routing in `read_model` + a field on `ModelData` (e.g. `roll`).
- [ ] A clip that rotates the ROOT `body` bone somersaults the whole model (children inherit) —
      use this for rolls/flips; child-bone keys (head/arms/legs) then tuck *relative* to it.
