# Ideas from other ports

What other Xbox recomps have done that Titanium Legion can use, picked from the knowledge base's surveys (xbox-recomp-kb: `guides/survey-2026-11.md` and `guides/thps2x-adoption.md`). Last updated 2026-11-01.

Titanium Legion is MIT, so code may come only from MIT (or compatible) projects, with a line in `NOTICE`. GPL projects and projects with no licence are read for ideas only.

## The frame-rate work: nobody has it yet

The cockpit runs at 5–7 fps because the shadow-map pass (depth only, float Z16), the scene pass sampling it with the NV2A's shadow compare, and the float-depth back-buffer draws all go to the CPU.

As of 2026-11-01, **no other renderer implements shadow compare or float depth on the GPU**. Checked:
- THPS 2X's Vulkan renderer (`nv2a_vk`);
- apexJCL's D3D11 and Metal back ends;
- Takedawn's D3D11 and Vulkan back ends;
- antbox's new `nvr` renderer.

So this is ours to build. Two things from others help with it:

- **Frame capture and replay** (antbox `src/nvr/nvr_capture.[ch]`, `tools/nvr_replay`, MIT). It records every pushbuffer kick plus the guest pages the title changed, then replays the frames headless through any back end and compares pixels. Capture Mission 00's cockpit once, then iterate on the GPU path without driving the game to the engine start every time.
- **Depth formats and bias**, from THPS 2X's lessons (`docs/lessons_learned.md` in almost-drowned-rat/xboxrecomp-vulkan):
  - D24S8 isn't available everywhere (AMD), so plan a D32-float fallback.
  - Polygon offset (methods `0x0330–0x0338`, `0x0384/0x0388`) must be converted per depth format.
  - Keep a switch that forces each fallback on the dev GPU, so it gets tested.

## Do next

1. **Lifter fix: jump tables indexed downward.** MSVC's `memcpy`/`memmove` jump through a table indexed *below* its address. Our disassembler counts upward and decodes the rest of the table as instructions; on another title that cut `memcpy` short.
   - Source: xenophobentx/xboxrecomp (Takedawn's toolkit) commit `0a7d117`, about 40 lines in `tools/disasm/engine.py`. MIT.
   - Our toolkit copy doesn't have it. Needs a re-lift.
2. **Pad + keyboard together, and scripted input:**
   - **Remapping references:** THPS 2X's toolkit `src/input/xinput_bind.c`, which is pure logic, keyboard and pads, with a lock-out rule so menus stay usable. Burnout 2's `src/input.cpp` has four players and full analog range. Both MIT.
   - **Steel Battalion's controller** has far more controls than a pad, so a text binding format like THPS 2X's (`control:source+source,...`) scales better than fixed tables.
   - **If the input layer moves to SDL3**, Burnout 2 notes that SDL drops joystick events while the window is unfocused unless `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is set. Set it only when an input script is given.

## Worth doing

- **F1 debug overlay** (BearddOddity/recomp-gui-template, MIT): an immediate-mode panel with FPS, CPU, GPU and memory, hooked on the D3D11 `Present` with no toolkit edits. Handy while measuring the cockpit passes.
- **RetroArch shaders on D3D11** (apexJCL `nv2a_present_chain.c`, MIT, with their D3D11-only librashader build). A CRT preset suits the cockpit monitors.
- **Atomic save writes** and **version + system on the first log lines** (THPS 2X's toolkit, MIT).
- **A readable start-up failure:** when the GPU path can't start, say which card and what's missing rather than running on silently (THPS 2X, v0.9.3).
