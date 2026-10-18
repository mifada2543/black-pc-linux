# TODO

Open work on the BLACK port. Findings and addresses live in `NOTES.md`; this is the short list.

**Everything below the Linux section is the archived Windows port's list**, kept because it records what was
still unverified when the Linux work started. The Windows sources are in `loader/windows/` and are not in
git, so none of it is actionable without that folder.

## Linux port (the actual work now)

All planning lives in `.agents/plan.md` — **one document** since 18 October 2026 (the plan, the phase gates,
the risk register, the full coverage list: workstreams A-G plus E, F, P, and the milestones, and the
decision log). This file is only the order, and `plan.md` decides which steps can be proven without
`game/default.xbe`.

1. ~~**D3D8 foundation.**~~ **Done 2026-10-18**: the ABI as a Linux header (`loader/src/d3d/xd3d8_posix.h`,
   every size/offset pinned twice), the parameter-spec thunk builder (`loader/src/d3d/thunk_posix.cpp`, 83
   checks through a hand-written caller), the device object plus the arrays the game writes in place
   (`loader/src/d3d/device_posix.cpp`), a table builder that reports every unbuildable row by name
   (`hle_table_posix.cpp`), and 12 state functions callable through their thunks (`hle_device.cpp`,
   `tests/test_hle_device.cpp`, 58 checks). Reasons and the two bugs this caught: `NOTES.md`,
   "Temuan Linux Track A".
2. **SDL2 host, window first.** `loader/src/host/host_sdl.*` does not exist yet, while SDL2 i386 and
   `src/gfx/png.cpp` are already here -- so this is the next step that can actually be *proven* on this
   machine (window + clear + PNG screenshot with `SDL_VIDEODRIVER=dummy`), and it comes before the draw
   path because the draw path's gate cannot pass here (llvmpipe 32-bit rasterises 0 fragments). Then input
   and audio output, replacing the XAudio2/XInput layer of the archive.
3. **Draw path over `src/gfx`** (roadmap C-2/C-3): vertex declaration, MANAGED textures via staging +
   dirty tracking, `Draw*UP`, readback. The details worth not losing are in `plan.md` §5.4 (C-2/C-3), not here.
4. **NV2A microcode**: the shader translation path can be built and tested with synthetic microcode, but the
   *real* format needs the image -- that stays Track B in `plan.md`.
5. **The Phase 1 gate**: run the real `default.xbe` to the first D3D call. Everything that can be verified
   without the image is verified: **20 suites, 1164 checks**, clean rebuild, zero warnings.


## Verify (fixed, waiting for a playtest)

- [x] Fades: the game's gamma ramp (fade factor), now applied at present (`ramp_texture` in loader/windows/src/d3d/device.cpp).
      User confirmed the fade works, and the in-game brightness slider now works too.
- [ ] Picture vs the Xbox Series: compare screenshots of the same still screen with the same brightness slider setting
      (method in NOTES.md, "To check"). Also check the other fades: loading screen before missions, mission start after
      the intro (played and skipped), intro cinematic -> loading screen, level-end trigger -> fade to black.
- [ ] Anything else that is timed in seconds (blur ramps, objective text typing, HUD animations) now runs at the
      right speed.
- [ ] Higher internal resolution (`--scale N`, default 3 = 2560x1440 widescreen, window fitted to the screen,
      DPI-aware): check that every effect still lines up (light shafts, reload blur, bloom, fades, video, menus) at 1-4.
- [ ] Widescreen (default; the game's own 16:9 mode, `--normal-aspect` for 4:3): menus, HUD, videos, briefings.
- [ ] Window resizing at a fixed aspect, fullscreen (PC SETTINGS), black bars when maximised.
- [x] Mouse look: rotates the view directly (user: "feels awesome").
- [ ] Seams of destructible objects: vertex snap 1/8 Xbox pixel + seam fill pass (PC SETTINGS > DISPLAY); user to confirm.
- [ ] Weapon sway with mouse look (the gun swivels like with the stick): user to check the feel and amount.
- [ ] Auto-aim switch (PC SETTINGS > AUDIO AND GAME; default off since 2026-10-12, `--autoaim`): confirm in play that
      the view no longer drifts onto enemies with it off, and that nothing else changes.
- [ ] Key bindings (keys bound to the game's actions, PC SETTINGS > CONTROLS > KEY BINDINGS): play with them; Esc pauses and frees
      the mouse, resuming takes it again. The game's on-screen button prompts still show pad buttons.
- [ ] Aspect ratio / frame rate (main menu PC SETTINGS > DISPLAY only): the choice shows NEXT START, is saved for the
      next run, and nothing changes until then (confirm in play).
- [ ] PC SETTINGS pages (2026-10-12): every row on the main menu and in the pause menu, the sliders, KEY BINDINGS
      (scrolling, capture, reset), FULLSCREEN from the menu. The pause page loads into a small heap: open it in every
      mission (a null allocation for PseMenu.bin would hang the pause menu; NOTES.md).
- [ ] 60 fps (the default since 2026-10-12, `--fps 30` = original): world step 1/60 s, audio fine, weapon refire and reload blur now match
      30 fps (measured). Check the rest in play: other weapons/fire modes, animations, AI, effects, scripted scenes;
      60 became the default (user).
- [ ] 60 fps slowdowns in busy scenes (user; also on PS2): the port's CPU overhead on the one game thread is the limit
      (perf recorder, NOTES.md). Round 1 done (texture checks once per frame, faster vertex conversion, shader key reuse,
      XInput): test scene 10.8 -> 5.1 ms; busy scene 78% -> 99.5% game speed. Round 2: stable textures checked every 16th frame.
      Stutter at 100% game speed: fixed (vblank follows the display; user: "silky smooth").
      Weak laptop (i5-4300U, HD 4400): 73.5% at 60 fps; play at 30 there. Round 3: redundant shader/decl calls skipped, game on
      the last core with its sibling idle. Laptop crash at the first level (256-byte immediate-mode vertices on Intel) fixed;
      user played the first level through on the laptop. Re-record there at 30 and 60 fps to see what the changes gave. Next: VS constants only when changed (broke the
      picture, see NOTES), cache converted vertices in host buffers, log writes off the render thread, game CPU other than CPU 0.
- [ ] Skip any movie (default; Game menu, `--no-movie-skip`): mission movies later in the game, end-of-mission movies,
      credits.
- [ ] Field of view (PC SETTINGS > VIDEO, `--fov`): feel in play, aiming and scopes (aimed FOV = setting / zoom),
      cutscene transitions back to the player's view; gun and hands at the game's view (reload, weapon switch, melee).
- [ ] Reverb (bin 11 send into an FDN reverb): level and character against the Xbox (`--reverb-gain`).

## Next

- [x] Custom render resolution: `--resolution WxH | H | native`, fractional `--scale` (any size, not just multiples).
- [x] Render size changes while running: `native` follows the window (and fullscreen); PC SETTINGS sets it.
- [x] Faint grid in the game's blur at high render sizes: the small post-processing buffers keep their Xbox size.
      (Also checks: glow and light cones look as on the Xbox; nothing else renders into small targets.)
- [ ] Resolution: better downsampling when the render size is much larger than the window (box filter instead of
      one bilinear tap).
- [x] Rebindable keys: bound to the game's actions (PC SETTINGS > CONTROLS > KEY BINDINGS, black.ini).
- [x] Settings remembered between launches (black.ini: render size, AA, fps, aspect, fullscreen, gain/gamma, mouse, reverb).
- [ ] Also remember the window size and position.
- [x] Port options in the game's own menus: Options > PC SETTINGS and the pause menu (built at start from the user's
      files, src/fe/; NOTES.md "PC SETTINGS in the game's menus"). The Windows menu bar is gone.

## Audio

- [ ] Reverb: exact room from the DSP image (effect 16's state is DSP-format; the HLE uses I3DL2 values by ear).
- [ ] Bins 17/18 (effects 13/14 -> outputs 0xE20/0xE40) and 27-30 (delay effects 18-21) are not mixed.
- [ ] LFE (subwoofer) bin is dropped.
- [ ] 3D sound: cones, doppler and the per-voice rolloff curve are ignored (distance + pan only).
- [ ] Output often reaches full scale (soft limiter active); check voice/mix-bin headroom against the Xbox.

## Open questions

- [ ] Where the Xbox re-fits the viewport to a smaller render target (the light-shaft pass relies on it; the HLE
      re-fits on every SetRenderTarget, see `fit_viewport`).
