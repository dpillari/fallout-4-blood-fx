# BloodFX — GPU blood spray & pooling for Fallout 4 (any DX11 card)

Fluid-like blood without NVIDIA Flex/CUDA. Runs on NVIDIA, AMD, Intel and Steam Deck.

## How it works
| Stage | Where | Cost |
|---|---|---|
| Hit / death event → queue a burst (position, shot direction, damage-scaled count) | CPU, game thread | ~0 |
| **Emit** compute shader writes drops into a ring buffer (oldest recycled — no allocation, no readback) | GPU | 1 tiny dispatch |
| **Update** compute shader: gravity, drag, collision against the **screen depth buffer**; reconstructs surface normal, bounces/runs down walls, sticks on floors | GPU | 1 dispatch, 4096 threads |
| **Render**: flying drops = velocity-stretched billboards (look like streaks); stuck drops = irregular splats aligned to the surface that spread out, then fade | GPU | 1 draw call, no vertex buffer |

At the default 4096 particles this is well under 0.2 ms on low-end GPUs. Nothing is ever read
back to the CPU, so there are no GPU stalls.

**Limitation of depth-buffer collision:** drops only collide with surfaces that are on screen.
A drop that flies off-screen keeps falling and just expires. Pools that leave the screen are kept
(they're stuck, so they don't need collision) and reappear when you look back.

## Download
Every push builds automatically: open the **Actions** tab → latest **Build** run → download the
**BloodFX** artifact. Its contents go into `Data\` (or install the zip with a mod manager):
```
F4SE/Plugins/BloodFX.dll
F4SE/Plugins/BloodFX.ini
```
Requires F4SE and Address Library for your game version (built against runtime 1.11.240).

## Build locally
```
git clone --recurse-submodules https://github.com/dpillari/fallout-4-blood-fx
cd fallout-4-blood-fx
xmake f -m releasedbg -y
xmake build -y
```

## Self-calibration
- Camera matrix layout and standard vs reversed depth are detected at runtime; the result is
  written to `Documents\My Games\Fallout4\F4SE\BloodFX.log`.
- The depth buffer used for collision is picked automatically (first engine depth target the size
  of the screen). All candidates are logged; override with `iDepthTarget` in `BloodFX.ini`.

## License
GPL-3.0 (required by CommonLibF4). Source must accompany any binary you redistribute.

## Known v0.1 compromises → v0.2 ideas
- Drawn at Present, so it sits over the HUD and Pip-Boy screen. It's hidden while menus are open.
  Moving the draw to just before the game's UI pass (hook the HUD render call) fixes this.
- Unlit (fixed color + fake highlight). v0.2: sample the game's lighting/ambient buffer for tint.
- Each drop is drawn on its own. v0.2 "goo" pass: render drop depth to a half-res target,
  blur, reconstruct normals → connected liquid surface (screen-space fluid). Still vendor-neutral.
- Blood color per race (ghouls, mirelurks, synths) via INI keyword → color table.
- Hook gib/dismember (see the gore torso notes) for a big burst + limb-stump dribble emitter.
