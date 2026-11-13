# Settings reference (fe2port / fe1port)

Everything the port reads: the settings file, the command line and the debug environment variables. Taken from
the source (`runtime/main.c` `load_settings` and `main`, `runtime/input.c`, `getenv` calls in `runtime/`), 3 Nov 2026.
If a key or option is added, add it here too.

## Where the settings file lives

One file per exe, **next to the exe** (`SDL_GetBasePath`), named after the game:

| Run from | File |
|---|---|
| Development build FE2 | `build\fe2port.ini` |
| Development build FE1 | `build_fe1\fe1port.ini` |
| Package FE2 ("(package)" shortcut) | `dist\fe2port\fe2port.ini` |
| Package FE1 | `dist\fe1port\fe1port.ini` |
| Debug builds | `build_dbg\fe2port.ini`, `build_dbg_fe1\fe1port.ini` |

- Missing file: written with all defaults on the first windowed start (`write_default_ini`).
- `tools/package.py` never overwrites an existing ini, so each folder keeps its own settings.
- **Headless runs (`--headless`) do not read the ini at all**: tests use the built-in defaults plus the command line.
- Command-line options override the ini. Unknown keys are ignored. Comments start with `;` or `#`. Values may be
  quoted (`disc = "F:/x/y.cue"`).
- An ini without a `[game]` section (older bindings-only files) runs with the defaults below; the first switch
  written by the menu adds a bare `[game]` section.
- Other files next to the exe: `memcards\card1.mcd` (memory card 1, all discs share it), `last_session.rec` (input of
  the last windowed session, rewritten each start; for `--replay`), `states\<game>_slot<N>.state` (save states,
  NOTES.md "Save states"; after a state load the recording starts over and the loaded state is copied to
  `last_session.state`: `--load-state last_session.state --replay last_session.rec` repeats the session).

## `[game]` section

| Key | Values | Default | Meaning |
|---|---|---|---|
| `disc` | path to Disc 1 `.cue` | empty | Used when no disc argument is given. Discs 2-4 are found next to it (same name with `(Disc N)`, or sibling folders `disc2..disc4`). Empty: the first `*(Disc 1)*.cue` in `<data>\..\discs\<game>\`. |
| `data` | folder | empty | Data folder (8x movies, models, texture packs, fonts, `ffmpeg.exe`). Relative paths are relative to the exe. Empty: `data\` next to the exe, else `..\data` (the repository's `build\` and `build_fe1\`). |
| `remaster` | empty / `off` / folder | empty | 8x backgrounds and character models. Empty = from the data folder, `off` = none, a folder = a remaster work folder (development). |
| `scale` | 1-8 | 8 | Internal resolution of the hw renderer (clamped to 1-8). 4 for weaker GPUs. |
| `renderer` | `hw` / `sw` | `hw` | OpenGL renderer or the software rasteriser (no remaster, no effects). Falls back to `sw` if OpenGL fails. |
| `cd_speed` | integer >= 1 | 8 | Data reads this many times faster than the CD drive (movies and XA voices keep the real rate). 1 = original loading times. |
| `smooth` | 0 / 1 | 1 | 60 fps remodels (posed between the game's 30 fps frames). 0 = the game's 30 fps. |
| `bloom` | 0 / 1 | 1 | Bloom (glow of the brightest lights). |
| `bloom_intensity` | float | 0.80 | Bloom strength. |
| `bloom_threshold` | float | 0.7 | Brightness above which bloom starts. Not in the default file. |
| `bloom_radius` | float | 1.0 | Bloom blur radius. Not in the default file. |
| `bloom_halo` | float | 0.5 | Wide halo share. Not in the default file. |
| `vignette` | 0 / 1 | 1 | Darker corners. |
| `vignette_strength` | float | 0.30 | Vignette darkness. |
| `vignette_start` | float | 0.45 | Distance from the centre where it starts. Not in the default file. |
| `vignette_softness` | float | 0.55 | Width of the transition. Not in the default file. |
| `grain` | 0 / 1 | 1 | Film grain. |
| `grain_strength` | float | 1.00 | Grain amount. |
| `grain_rows` | float | 640 | Grain size (rows of grain over the screen height). Not in the default file. |
| `grain_refresh` | float | 24 | Grain pattern changes per second. Not in the default file. |
| `loop_fade` | movie frames (15 per second), 0 = hard cut | 8 | Room loops: the end of the loop dissolves into its start over this many frames. The user runs 4. |
| `scene_look` | a look name from actors.txt | `b` | Which tagged scene-actor lines draw (actors.txt `look=NAME`; the cog wheels' `b` (pre-rendered + HYPIR sprites, the user's pick, the default), `current` (the 5 Nov meshes), `a` (shader look), `c` (the dead soldiers' painted look); a name no line has, e.g. `off`, hides them all; with no `b` line in actors.txt `current`, with neither the first look it names). `Ctrl+F6` cycles the looks live in actors.txt order (b, current, a, c), `--scene-look NAME` and `FE_SCENE_LOOK` (wins over both) set it. Not in the default file. |
| `movie_60fps` | 0 / 1 | 1 | 60 fps room movies (a `movies.txt` line with a rate, NOTES.md "60 fps room movies") show their in-between pictures; 0 = only the STR frames' own pictures (15 fps). Not in the default file. |
| `movie_cam_fix` | 0 / 1 | 1 | In a camera move, the game's 3D characters are drawn with the camera of the movie picture on screen (`cam_tracks.txt` in the models folder, NOTES.md "60 fps room movies: camera-move scan"), so they glide with a 60 fps background instead of stepping once per movie frame; 0 = the game's own camera. Not in the default file. |
| `display` | `fullscreen` / `windowed` | `fullscreen` | Borderless fullscreen or a window. |
| `fill` | 0 / 1 / 2 | 1 | Screen layout in rooms: 1 = enlarged picture with the HUD in bands, 2 = HUD over the picture, 0 = original 4:3. Applies to every screen that shows the room picture, overlays such as the pause screen included; title, menus and other screens drawn without it stay 4:3 (NOTES.md "Screen layout: which screens"). |
| `models` | 0 / 1 | 1 | Remaster character models (F8 toggles). |
| `model_fix` | bits, 0 = off | 251 | Look fixes of the remodels (NOTES.md "Remodel look fixes", `runtime/mlook.h`); `Ctrl+F8` toggles them off / on live. 1 = the game's lit colours blended over the surface (no flat per-face patches with saw-tooth edges, the "bristles"); 2 = faces the game culled this frame take their neighbours' colour (no blinking patches on the nose, neck and chin); 4 = each part pinned to its parent's joint (off by default: measured worse than 8); 8 = the parts' whole-unit rounding low-passed while she moves slowly (the head's twitch at every idle loop); 16 = outline: thicker silhouette, thinner inner lines, at least ~1.5 screen pixels; 32 = cel bands from smoothed normals (round shadow shapes, more on the face); 64 = sheen band on dark fabric; 128 = the improved meshes of `swap_look.txt` in each game's models folder (`tools/model_look.py`: FE2 Rain's four costumes and her scene-actor underwear, Hana's five, Glas Indy / wounded / camo; FE1 Hana's four, Glas, Glas wounded). 0 = the look before 5 Nov. Not in the default file. |
| `directional` | 0 / 1 | 1 | Directional controls (stick direction = screen direction; L1 held = tank). 0 = original tank controls. Only while the game updates the player in a room: puzzles, menus, pause, game over and cutscenes get the d-pad as it is (NOTES.md "Directional controls: puzzles and menus"). |
| `textures` | empty / `off` / folder | empty | Texture packs (redrawn interface, F6 toggles): empty = the data folder's, `off` = none, a folder = the folder holding the `SLUS-xxxxx` folders. |
| `fx_particles` | 0 / 1 | 1 | Effect particles drawn smooth (hw renderer, presentation only; F6 switches it off and on with the pack): blood sprays as round droplets, shell casings as rounded bars, a soft glow under bullet-hit shards (NOTES "Effect textures"). On by default since the merge of 5 Nov (the user: "Merge, particles on"); 0 = the game's own particles. When the ini has no `fx_particles`, `FE_FX_PARTICLES=0` / `1` sets it (the ini key wins). Not in the default file. |
| `precision` | 0 / 1 | 0 | Precise (PGXP-style) vertices, experimental. Not in the default file. |
| `volume` | 0-100 | 100 | The port's output volume in percent (on top of the game's own master volume; also in `--wav` dumps). Not in the default file. |

The options menu (Options on the game's main menu; NOTES.md "Options menu") writes `volume` (Sound), `directional`
(Controls), `fill`, `smooth`, `display`, `bloom`, `bloom_intensity`, `vignette`, `vignette_strength`, `grain`,
`grain_strength` (Visuals) into this section when they are changed there. Its Remaster switch and the hotkeys act on
the running session only and are not saved. The game's own options (sound, difficulty, subtitles, vibration, FE2's
control type / button layout / auto draw / save beacon, FE1's button configuration) are not in this file: they live
in the game's RAM and are saved by the game on the memory card, as on the PlayStation.

## `[keyboard]` and `[controller]` sections (bindings)

Rewritten by the options menu (Controls: Keyboard / Gamepad bindings); hand edits work. Several inputs per action: separate
with commas. Keyboard names are SDL key names (`Return`, `Space`, `Left Shift`, `Keypad 8`, `F9`). Controller names
are SDL's, by position in Xbox naming (`a` = bottom button = Cross): `a b x y back guide start leftstick rightstick
leftshoulder rightshoulder dpup dpdown dpleft dpright misc1 touchpad paddle1-4`, plus `lefttrigger` /
`righttrigger`.

| Action key | Keyboard default | Controller default |
|---|---|---|
| `up` / `down` / `left` / `right` | arrow keys | `dpup` / `dpdown` / `dpleft` / `dpright` |
| `cross` / `circle` / `square` / `triangle` | `S` / `D` / `A` / `W` | `a` / `b` / `x` / `y` |
| `l1` / `r1` | `Q` / `E` | `leftshoulder` / `rightshoulder` |
| `l2` / `r2` | `1` / `3` | `lefttrigger` / `righttrigger` |
| `l3` / `r3` | `Z` / `C` | (none: the controller's L3/R3 carry port actions) |
| `start` | `Return` | `start` |
| `select` | `Backspace` | (none) |
| `lstick_up/down/left/right` | `I` / `K` / `J` / `L` | (none: the controller's sticks are always the DualShock sticks) |
| `rstick_up/down/left/right` | `Keypad 8` / `Keypad 2` / `Keypad 4` / `Keypad 6` | (none) |
| `analog` | `F9` | `guide` (PS button) |
| `act_controls` (directional / tank) | `F7` | `leftstick` (L3) |
| `act_remaster` (remaster on / off) | `F12` | `back, touchpad` (Select / Create, touchpad) |
| `act_overlay` (screen layout 1 -> 2 -> 0) | `F10` | `rightstick` (R3) |
| `act_fullscreen` | `F11` | (none) |
| `act_state_save` (save state into the current slot) | `F5` | (none) |
| `act_state_load` (load the current slot's state) | `F8` | (none) |
| `act_state_next` / `act_state_prev` (state slot 1-9) | `PageUp` / `PageDown` | (none) |

The port's actions (`act_*`) do not fire from the keyboard while Shift, Ctrl or Alt is held (Shift+F8 and Alt+Enter
are fixed keys). Inis written before 4 Nov have no `act_state_*` lines: the defaults above apply.

Extra keys of `[controller]`:

| Key | Values | Default | Meaning |
|---|---|---|---|
| `deadzone` | 0-32767 | 8000 | Stick deadzone. |
| `analog_on_start` | 0 / 1 | 1 | Start with the ANALOG light on (the games read the left stick only in analog mode). Overridden by `--analog` / `--digital`. |
| `stick_as_dpad` | 0 / 1 | 0 | The left stick also presses the d-pad (menus, digital mode). |

Fixed keys (not bindable): `F1`-`F4` insert disc 1-4 by hand, `Alt+Enter` fullscreen, `F6` redrawn interface only,
`Shift+F8` remaster characters only (`F8` alone until 4 Nov; `F8` now loads a save state), `Ctrl+F8` the remodels' look
fixes off / on (`model_fix`), `Ctrl+F6` the scene actors' next look (`scene_look`; the cog wheels' styles).

## Command line

`fe2port.exe [<Disc 1 .cue>] [options]` (same for `fe1port.exe`). Any argument that is not an option is the disc.

**Play options**

| Option | Meaning |
|---|---|
| `--scale N` | Internal resolution 1-8 (ini `scale`). |
| `--renderer hw\|sw` | Renderer (ini `renderer`). Default hw in a window, sw headless. |
| `--windowed` / `--fullscreen` | Window or borderless fullscreen (ini `display`). |
| `--fill 0\|1\|2`, `--no-fill` | Screen layout (ini `fill`); `--no-fill` = 0. |
| `--cd-speed N` | Loading speed (ini `cd_speed`). |
| `--no-smooth` | Remodels at 30 fps (ini `smooth = 0`). |
| `--no-fx` | No bloom, vignette or grain. |
| `--loop-fade N` | Room loop dissolve in movie frames, 0 = hard cut (ini `loop_fade`). |
| `--scene-look NAME` | The scene actors' look (ini `scene_look`; actors.txt `look=`). |
| `--no-movie-60fps` | 60 fps room movies at 15 fps (ini `movie_60fps = 0`). |
| `--no-movie-cam-fix` | Characters with the game's own camera in camera moves (ini `movie_cam_fix = 0`). |
| `--pgxp` / `--no-pgxp` | Precise vertices on / off (ini `precision`). |
| `--no-dither` | No PS1 dithering. |
| `--data DIR` | Data folder (ini `data`; a relative path is relative to the current directory here). |
| `--remaster DIR` / `--no-remaster` | Remaster folder / no 8x pictures and models (ini `remaster`). |
| `--no-models` | No remodelled characters. |
| `--textures DIR` / `--no-textures` | Texture pack folder / none (ini `textures`). |
| `--tank-controls` | Original tank controls (ini `directional = 0`). |
| `--analog` / `--digital` | Force the DualShock mode at start. |
| `--memcard FILE` | Memory card 1 image (default `memcards\card1.mcd` next to the exe). |
| `--no-autoswap` | No automatic disc changes (F1-F4 still work). |
| `--cheats FILE.cht --cheat NAME` | DuckStation cheat file; `--cheat` (up to 16, part of the name) enables entries. |
| `--mute` | No sound. |
| `--list-controllers` | Print SDL's controllers and exit. |
| `--input-test` | Small window printing the PS1 buttons and sticks the port sees. |

**Test and debug options**

| Option | Meaning |
|---|---|
| `--headless` | No window, no sound device, no ini. Software renderer unless `--renderer hw`. |
| `--fast` | No frame pacing (headless: ~1000 frames/s; waits for 8x movie frames). |
| `--fast-until FRAME` | Unpaced up to FRAME, then 60 Hz (windowed timing tests). |
| `--frames N` | Stop at frame N; writes the RAM to `out/ram_end.bin` (or `FE_RAM_END`). |
| `--dump-every N`, `--dump-from F`, `--dump-dir D` | PNG of the display every N frames from frame F into D (`frame_NNNNNN.png`). |
| `--dump-screen WxH` | With the dumps, also the composed window picture at WxH (`screen_NNNNNN.png`). |
| `--log FILE`, `-v` | Log file; `-v` = verbose. |
| `--wav FILE` | Write the sound output to a WAV. |
| `--record FILE` / `--replay FILE` | Record / replay the pad state of every frame (windowed sessions record to `last_session.rec` by default). |
| `--press FRAME:MASK[:LEN]` | Scripted buttons (MASK hex in PS1 bit order, LEN frames, default 6; up to 64). |
| `--stick FRAME:LX:LY[:LEN]` | Scripted left stick (0-255, LEN default 60; up to 64). |
| `--fuzz SEED[:FROM]` | Random input from frame FROM (soak tests). |
| `--agent` | Headless only: the port as a step-by-step environment over stdin / stdout for the playtest tools (`tools/playtest/README.md`; commands in `runtime/agent.c`). `FE_AGENT_TXT=1` logs the FE2 HUD texts it sees. |
| `--load-state FILE` | Start straight from a save state (any disc of the set; the disc argument only names the set). With `--replay` the replay goes on at the state's frame. A state the build cannot use ends the run with `FATAL: --load-state ...: <reason>`. |
| `--save-state-at FRAME:FILE` | Save a state at the first state point (VSync in the game's main thread) at or after FRAME (up to 16). A FRAME at or before a `--load-state` frame saves right after that load (round-trip checks). |

## Environment variables (debugging)

All optional; set them before the start (`$env:FE_PERF=2` in PowerShell). They log into `--log` / stderr.

**Timing and movies**

| Variable | Meaning |
|---|---|
| `FE_PERF=1` | Per second: worst frame time and audio queue level. |
| `FE_PERF=2` | Adds a per-frame breakdown of every frame over 20 ms. |
| `FE_DBG_MOVIE=1` | 8x movie decoder: standby start, first frame times, held presents, stand-in buffers. |
| `FE_MOVIE_NO_STANDBY=1` | Old cold ffmpeg start per movie (no standby decoder), for comparison. |
| `FE_MOVIE_CACHE=N` | Decoded 8x frames kept per movie (16-512, default 96). |
| `FE_DBG_HFR=1` | 60 fps room movies: each in-between picture put into a movie buffer (pass / in-between, picture number), or held; "present frame N shows picture P" per presented vblank; "loop end" when the game seeks back to a 60 fps loop's start. `tools/research/loop_hold.py` reads it. |
| `FE_LOOP_HOLD_OLD=1` | 60 fps loops: the loop's last STR frame, which the game holds 8 vblanks before the restart, shows its pictures at the usual 1 a vblank and then its last picture for 5 vblanks (the freeze before 6 Nov), instead of spreading them over the hold (NOTES.md "60 fps loops: the hold before the restart"). For comparisons. |
| `FE_CAPTURE=FROM:TO` | Every frame the window presents in that range as PNG (hw, windowed; up to 256 frames, written at the end). |
| `FE_CAPTURE_DIR=D` | Folder for `FE_CAPTURE` (default the current directory). |
| `FE_DBG_SMOOTH=1` | 60 fps remodels: in-between frame log. |
| `FE_DBG_CD=1` | Movie stream starts (sector, frame). |
| `FE_DBG_PIECES=1`, `FE_NO_PIECE_FIX=1` | Movie seam at PS1 x = 256: log / turn the fix off. |
| `FE_DBG_LAYOUT=1` | Screen layout: each change of display, draw area, movie stream, room-picture test and layout at the presented frames. |
| `FE_LAYOUT_OLD=1` | Screen layout as before 5 Nov: only while a movie streams (the pause screen fell back to 4:3), for comparisons. |
| `FE_DBG_UPLOADS=FRAME` | Log VRAM uploads for 40 frames from FRAME. |
| `FE_MDEC_DUMP=1` | Dump MDEC decodes. |

**Models, props, actors, textures, subtitles**

| Variable | Meaning |
|---|---|
| `FE_DBG_MODELS=N` | Remodel log (level N). |
| `FE_NO_MODELS=1` | No remodels (like `--no-models`). |
| `FE_MODEL_FIX=N` | The remodels' look fixes (bits as the ini's `model_fix`; 0 = the old look). Overrides the ini. |
| `FE_MODEL_DBG=N` | Remodel shading parts alone (diagnosis): 1 the texture, 2 the game's light, 3 the cel bands. |
| `FE_DBG_BONES=NAME:FILE` | Every drawn pose of model NAME (each part's GTE view bone, 60 fps in-betweens marked 9) to FILE (the tremble study). |
| `FE_DBG_REC=NAME` | Log the part records of model NAME (first 80). |
| `FE_MODELS_OLD_IDENT=1` | Remodel part identification as before 5 Nov (faceless parts unposed, their next part or skin lost for 60 frames at a time; skins on actors of their own): for comparisons (NOTES.md "Faceless parts"). |
| `FE_DBG_SHADOW=1` | Shadow / mask log. |
| `FE_DBG_ACTORS=1` | Scene actors: a character's root in world terms every 30 frames (calibration). |
| `FE_ACTOR_CAM_OLD=1` | Scene actors drawn with the RAM camera of every pass, as before 6 Nov (they jumped at a camera cut while the old view's picture was still up): for comparisons (NOTES.md "Scene actors: the camera of the picture on screen"). |
| `FE_DBG_ACTCAM=1` | Scene actors' camera per pass (the picture's run, RAM eye, the eye used, held / taken) and where each drawn actor's origin lands; `tools/research/actor_cut_jump.py` reads it. |
| `FE_DBG_SPRITES=1` | Sprite actors (actors.txt `sprite=`): each draw's angle, the two poses and their blend, the first corner on screen. |
| `FE_CROWD_OLD=1` | Scene actors and remodels as before 7 Nov (NOTES.md "Scene actors: crowd capacity"), all of the three below at once: for comparisons. |
| `FE_ACTORS_OLD=1` | actors.txt read into the old fixed list of 64 lines (the rest dropped, now with a warning per line) and every line compared by movie name each pass, instead of the grown list indexed by movie. |
| `FE_POSE_RING_OLD=1` | The 60 fps pose store as before: 128 slots and an in-between's next pose found by searching the whole store, instead of 512 slots and each pose linked to its mesh's next. |
| `FE_AA_FULL=1` | Each remodel's anti-aliasing work (clear, resolve, composite) over the whole drawing area, as before, instead of over the mesh's own box on screen. |
| `FE_MATTE_OFF=1` | Scene mattes (actors.txt `MATTE` lines) not used: the actors are drawn over the whole picture, as without them. |
| `FE_DBG_MATTE=1` | Scene mattes: per pass (first 200) the view, its matte texture and where the movie picture lies in VRAM. |
| `FE_SKIN_DQ_OFF=1` | Scene actors with `skin=dq` skinned with the linear blend like every other mesh (comparisons). |
| `FE_DBG_SKIN=FILE`, `FE_DBG_SKIN_ACTOR=N`, `FE_DBG_SKIN_FROM=VBLANK` | The first scene actor drawn (or actor line N of the list, from that vblank) writes its vertices as the GPU skins them (transform feedback: `gl_Position`, depth) with the bones and settings to `FILE.bin`, its mesh to `FILE.txt`, once; `python tools/skin_ref.py check FILE` compares them with the Python reference. The mesh program is linked for the capture only while this is set. |
| `FE_GPU_TIME=1` | Measuring mode: every vblank a `glFinish`, the GPU time of the scene actors (the game's passes) and of the 60 fps in-between rebuilds read with timer queries, logged every 240 vblanks (`gputime:` lines, with the remodel draws a vblank and the wall time); headless it also composes the window picture every vblank so the in-betweens are built as in a window (`tools/research/crowd_check.py bench`). |
| `FE_POSE_REC=NAME:FILE` | Record model NAME's poses to FILE (scene actors). |
| `FE_DBG_PROPS=FILE` | Props inventory: every PS1 object drawn, written to FILE (`tools/props_inventory.py`). |
| `FE_PROPS_ROOMS=FROM[:EVERY[:FIRST[:LAST]]]` | Props inventory: walk through the rooms from frame FROM. |
| `FE_DBG_TEX=1`, `FE_DBG_TEX_FROM=FRAME` | Texture pack matches; every textured draw for 3 frames from FRAME. |
| `FE_DBG_PRIM=FROM:TO` | Every GPU drawing command of frames FROM..TO in the log (op, raw words, draw offset, texture page). |
| `FE_DBG_FX=FILE`, `FE_DBG_FX_PRIM=FROM:TO` | Effect texture scraper log (`runtime/fxlog.c`, read only): every upload with its XXH3-64 (pixels once per hash in FILE.bin), per frame every textured or untextured polygon, sprite and line summed by page / CLUT (untextured: colour in 8 levels) / blend (tools/fx_textures.py). |
| `FE_MODELS_SKIN_MESH=1` | Remodel vertices weighted to a skin part are drawn as before 5 Nov instead of left out with the game's skin kept (FE2's flood gate FLDGATE: the panel collapses to one edge): for comparisons (NOTES.md "Flood gate"). |
| `FE_NO_CODEPANEL=1` | FE2 code door panel: the game's own key cursor and press flash instead of the port's (NOTES "Code door panel"). |
| `FE_DBG_SUBS=1` | Native subtitles log. |
| `FE_DUMP_FONTS=1` | Write the font index map as a PGM. |

**Engine, tracing, tests**

| Variable | Meaning |
|---|---|
| `FE_TRACE=all` or `FE_TRACE=addr,addr` (hex) | Log calls of all / these recompiled functions. |
| `FE_TRACE_FROM=FRAME` | Tracing from FRAME on. |
| `FE_TRACE_MEM=N` | With a traced call, dump N bytes at a0. |
| `FE_TRACE_RET=1` | With a traced call, also log the return values. |
| `FE_TRACE_FILES=FILE` | Append every host file the port opens to FILE. |
| `FE_WATCH=LO[,HI]` (hex) | Log RAM writes in that range (only in a build with `tools/build.py --watch`). |
| `FE_RWATCH=LO[,HI]` (hex), `FE_RWATCH_FROM=FRAME` | Log RAM reads in that range with the reading function (watch build only; e.g. which code reads the pad block). |
| `FE_DBG_CONTROLS=1` | Directional controls: per frame with a direction held, whether the player is on the pad (or another loop owns it), off view, steering; `=2` also each update of the player. |
| `FE_CONTROLS_OLD=1` | Directional controls as before 4 Nov (steering also while the player is not updated), the new rule only logged: exact replays of older sessions for comparisons. |
| `FE_CONTROLS_OLD_REAPPLY=1` | Directional controls: the facing put back at the vblank over any value the game wrote (as before 5 Nov, night; it turned Hana round on a ladder mount): comparisons only. With `FE_DBG_CONTROLS=1` each vblank where the game changed the steered facing is logged (`vblank facing ... -> the game's / ours again`). |
| `FE_DBG_PLAYER=1` | Per vblank: the player's position, facing, state address and flags (0x4000 = on a ladder), the pad, the steering decision (read only); `=2` adds the actor's and the state's raw words. |
| `FE_DBG_GAMEFIX=1` | Log each time a fix for an original game bug acts (`runtime/gamefix.c`; FE2's area A tile puzzle). Its repairs of loaded states are always logged. |
| `FE_NO_GAMEFIX=1` | The original games' own bugs as they are (comparisons with older builds; NOTES.md "Crash after the keypad"). |
| `FE_DBG_RING=1` | FE2 movie ring-buffer hook (`StGetNext`). |
| `FE_DBG_ETC=1` | libetc state in exception logs. |
| `FE_DBG_MODULE=1` | FE1 module changes (module word, wanted disc). |
| `FE_RAM_END=FILE` | Where `--frames` writes the RAM (default `out/ram_end.bin`). |
| `FE_DUMP_SW=1` | With frame dumps, also the software renderer's picture (`sw_NNNNNN.png`). |
| `FE_MENU_TEST=1` | Let the options menu open in headless runs (`--renderer hw`; without it headless runs keep the game's own options screens). |
| `FE_MENU_KEYS=frame:scancode,...` | Type keys into the menu in headless runs (81 Down, 82 Up, 79 Right, 80 Left, 40 Enter, 41 Esc). |
| `FE_DBG_OPTIONS=1` | Options menu: each change of the game's shell state (FE2 node / target / cursor, FE1 module / cursor), each game option written, each sound apply. |
| `FE_STATE_HASH=FRAME` | Save states: log per vblank from FRAME the hashes of RAM and VRAM and the cycle count (`hash N ram ... vram ... cyc ...`), for determinism checks. |
| `FE_STATE_LOAD_AT=FRAME:FILE` | Load a state at that vblank of the running game, as the load key does (tests of the in-game load). |
| `FE_DUMP_NO_VRAM=1` | With frame dumps, no `vram_NNNNNN.png` (for every-frame dumps of long ranges). |
| `FE_DUMP_NO_FRAME=1` | With `--dump-screen`, no `frame_NNNNNN.png` (the screen pictures only). |

## Data files: scene actors (`data\<game>\models\actors.txt`)

One `ACTOR` line per live figure or decoration drawn over a room movie (format and the character options in
`SCENE_ACTORS.md`). Options for room decorations (the FE2 machine room's cogwheels, NOTES "Cogwheel room"), all on the
game's clock (vblanks / 60), presentation only:

| Option | Meaning |
|---|---|
| `spin=X,Y,Z,DEG` | The root turns about that model axis at DEG degrees a second. |
| `osc=AMP,STROKE,RAMP,DWELL,DELAY` | The root turns AMP degrees (sign = the first stroke's way) about the spin axis (default model z) and back: STROKE seconds a stroke, sinusoidal eases of RAMP seconds at both ends, DWELL seconds held at each end, shifted DELAY seconds. A gear train: one line per wheel, same timing, amplitudes in the tooth ratio. |
| `phase=DEG` | A fixed turn about that axis (a gear's starting tooth position). |
| `lights=FILE` | A lit colour per movie frame ("R G B" lines, 128 = 1.0): the room light baked into the movie. |
| `ink=0`, `shade=V` / `shade=smooth:V` | No ink outline; this mesh's SHADE instead of swap.txt's. |
| `sprite=FILE.spr` | Drawn as pre-rendered pictures chosen by its angle (two neighbouring poses turned to the exact angle and cross-faded), the mesh only if the pictures are missing; with `lights=` the pictures scale by each frame's colour over the file's median (SCENE_ACTORS.md "Sprite actors"). |
| `matte=0` | Not hidden by its view's matte (below): an actor that is always in front. |
| `skin=dq` | Dual-quaternion skinning instead of the linear blend (11 Nov, NOTES "Scene actors: dual-quaternion skinning"): vertices blended between two bones keep their distance to the joint when it bends far (the linear blend pulls them in: the shoulders and elbows of raised arms collapse). Needs a uniform `scale=` (a non-uniform one falls back to the linear blend, logged). Every mesh without it, the game's characters included, is skinned exactly as before. `tools/skin_ref.py` is the same maths in Python. |

The list has no fixed size (it had 64 lines before 7 Nov; a line that cannot be used is logged with its line number
and counted in the `actors: N ACTOR lines, M in use, K dropped` summary at start).

**Mattes** (7 Nov, NOTES "Scene actors: crowd capacity"): `MATTE <movie> <first> <last> <file.matte>` gives the
foreground painted in a view's picture (a bar counter, a railing, a table) a depth, so live actors behind it are hidden
by it. The `.matte` file (text, path relative to the models folder):

```
MATTE1
mask <png> <z>                  covered pixels (alpha >= 128, or luma >= 128 without alpha) lie at view depth z
depth <png> <scale> [<bias>]    16-bit (or 8-bit) grey PNG: view depth = value * scale + bias where value > 0
```

Every image is the movie's 8x picture (2560 x 1280 for a 320 x 160 movie, top row first), all the same size; per pixel
the nearest layer counts. View depth is the GTE's: the distance along the camera's axis, `f . (P - eye)` with `f` the
unit vector from the camera's eye to its target (cameras.json). An actor's pixel whose depth (view z + its `zback`)
is greater than the matte's at that point of the picture is not drawn (its ink outline neither); the game's own models
never test against it. Read on a thread at the view's first appearance (its actors wait for it, a frame or two);
at most 4 matte textures (R16F) stay on the GPU. Not applied to sprite actors or to the actors' floor shadows.

## Tools (repository)

- `python tools/build.py --disc "<Disc 1 cue>" [--regen] [--debug] [--watch]`; FE1: `--config config/fe1.json`
  (`--debug`: gdb build in `build_dbg\`, `--watch`: RAM watchpoints in `build_watch\`).
- `python tools/package.py --game fe2|fe1 [--data]`: refresh `dist\<game>port\` (copies this file as `SETTINGS.txt`).
- `python tools/area_tests.py --game fe2|fe1 [--extra "--renderer hw"] [--only AREA] [--package] --jobs 2`: regression suite.
- `python tools/headless_lock.py status|release` (and `with headless(...)` from Python): one headless batch at a time
  on this machine.
