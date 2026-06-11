# TILEMAP Game Engine — Design

Status: design, not yet implemented.

A C-level game-loop driver layered on the existing TILEMAP subsystem
(`shared/gfx/Tilemap.c`) and FASTGFX. One command initializes the engine; one
command per frame paces, presents, simulates, detects collisions, and renders.
BASIC code is reduced to game logic: reacting to events, reading input, and
drawing the HUD.

The API shape follows the proven engine in the
[pico-gamer](https://github.com/jvanderberg/pico-gamer) console: velocity-driven
sprites updated by the engine each frame, per-sprite collision *response modes*
(detect / bounce / destroy / stop), collision groups, per-frame hit state, and
tile properties that make the map double as the collision world.

```basic
' content
FLASH LOAD IMAGE 1, "tiles.bmp"
TILEMAP CREATE mapdata, 1, 1, 16, 16, 16, 100, 15
TILEMAP ATTR attrs, 1, 64
TILEMAP SPRITE CREATE 1, 1, 7, 160, 120

' engine
TILEMAP GAME 60

DO
  TILEMAP UPDATE
  FOR i = 1 TO TILEMAP(EVENTS)
    ' react to collisions
  NEXT
  TEXT 4, 4, "SCORE " + STR$(score)      ' HUD: ordinary drawing
LOOP
```

## Motivation

The TILEMAP manual's own usage patterns (gravity, attribute-based collision,
sprite-pair hit testing) run per entity, per axis, per frame in interpreted
BASIC. Every primitive they call already exists as a C function
(`TILEMAP(COLLISION ...)`, `TILEMAP(SPRITE HIT ...)`, `blit121()`). The engine
moves that loop into C and exposes the results as per-frame events and status
flags. Velocities are specified in pixels per second so the same program runs
at the correct speed on every device regardless of achieved frame rate — the
in-tree pico_blocks demo carries per-device speed calibration in its header
comment, which is exactly the problem this removes.

## Frame pipeline — `TILEMAP UPDATE`

Each call runs, in order:

1. **Present** — swap the previous frame to the display (`bc_fastgfx_swap()`;
   on host also `bc_fastgfx_sync()`). Everything BASIC drew since the last
   UPDATE (event reactions, HUD) goes out with this present.
2. **Pace** — wait until the current frame's time slot. The engine owns
   pacing: an absolute schedule (`next_frame += period`) so per-frame overhead
   does not accumulate into drift, with a catch-up rule — when more than two
   frame periods behind (a `PAUSE`, file I/O, a stall), the schedule resyncs
   to now instead of bursting zero-wait frames (same rule as
   `host_fastgfx_resync_after_sleep`). The FASTGFX driver's own FPS limit is
   left unset so there is exactly one pacer. The wait polls the console abort
   check so CTRL-C remains responsive at low frame rates.
3. **Measure dt** — elapsed time since the previous step, carried in
   microseconds (64-bit intermediates; 24.8 fixed-point *seconds* would
   quantize to ~6% rate error at 60 fps), clamped to 100 ms. On a device
   holding the target rate, dt is exactly `1/fps` every frame, so physics is
   effectively fixed-timestep.
4. **Integrate** — for every active sprite: `v += a*dt`, `pos += v*dt`
   (semi-implicit Euler, fixed point — see Physics).
5. **Resolve solids** — per axis: move X, test the sprite's hitbox against
   its solid-attribute mask via the tile-collision test; on contact apply the
   sprite's **solid response mode** (see Response modes) — DETECT leaves
   motion untouched, BOUNCE reflects the velocity component, STOP snaps to
   the tile boundary and zeroes the component, DESTROY deactivates. Then the
   same for Y (a downward stop sets `landed`). Contact always sets status
   bits. If a sprite's displacement this step exceeds the tile size, the move
   is internally sub-stepped (capped count) to prevent tunneling.
6. **Apply edge policy** — per sprite at the world (map) bounds, per edge:
   free, clamp, wrap, bounce, or destroy. Edge contact sets status bits and
   emits an event.
7. **Detect** — all-pairs AABB (hitboxes) across active sprites, filtered by
   collision group masks, applying each sprite's pair response mode;
   event-tile test against each sprite's event-attribute mask. New contacts
   (absent last step) append to the event list. Continuous contact does not
   re-report; poll `TILEMAP(SPRITE HIT a, b)` or `TILEMAP(STATUS id)` for
   "still touching".
8. **Render** — clear the back buffer to the scene background colour, draw
   the scene's tilemap layers in order, then the sprite batch.
9. **Return** — BASIC drains the event list, draws HUD, loops.

`TILEMAP UPDATE dt_ms` (explicit timestep) runs steps 3–9 with the given dt
and skips the pacing wait — the deterministic mode for host tests and games
that manage their own time. `TILEMAP UPDATE 0` presents and renders without
integrating, resolving, or emitting events: a world-freeze for explosion
holds, death sequences, and pause screens (the Picovaders pattern of
presenting frames while the world stands still).

## Logical resolution and scaling

Games are written against the logical resolution declared in `TILEMAP GAME`
and are portable across every display the engine can map them onto. The
mapping rules:

- **The engine only ever upscales.** The scale factor is the largest of
  **1× or 2×** that fits both dimensions; the scaled image is centred and
  the remaining bands letterbox (black), on either or both axes.
- **If the logical resolution exceeds the display in either dimension at
  1×, `TILEMAP GAME` errors** (`Display 320x240 is smaller than game
  320x320`). There is no downscale mode: fractional scaling destroys pixel
  art (1-px features land between destination pixels), and cropping
  silently amputates HUDs. A game that doesn't fit doesn't load.

| Game | 320×240 LCD | 320×320 PicoCalc | 480×320 ILI9488 | 640×480 VGA/HDMI |
|---|---|---|---|---|
| 320×240 | 1:1 | 1× + bands | 1× + bands | 2× |
| 320×320 | refused | 1:1 | 1× + side bands | 1× + bands |
| 160×120 | 2× | 2× + bands | 2× + bands | 2× + bands |

Everything is logical: world rendering, sprite coordinates, and all HUD
drawing (the engine points the drawing system's resolution at the logical
buffer for the session, so `TEXT`/`BOX`/`PRINT @` land in game space and
scale with it). `TILEMAP(WIDTH)` / `TILEMAP(HEIGHT)` return the logical
size. Scrolling games should declare a modest viewport (the world can be
any size); only fixed-screen layouts bake in an aspect ratio.

Scaling costs almost nothing where it lands: the SPI driver's per-line
nibble→RGB565 expansion writes each pixel (and line) twice at 2×, and the
scanout present duplicates rows during its copy. The diff runs on the
logical buffer — a 2×-scaled game diffs a quarter of the bytes — and a
small logical buffer shrinks the FASTGFX footprint on large displays.

## Response modes

One enum, used for solids, sprite pairs, and world edges — the pico-gamer
model:

| Mode | Value | Solid tiles | Sprite pair | World edge |
|---|---|---|---|---|
| `NONE` | 0 | ignore | no event | sprite may leave world |
| `DETECT` | 1 | event + status only, motion untouched | event + status only | event + status only |
| `BOUNCE` | 2 | reflect velocity on the contact axis | reflect both sprites' contact-axis velocity | reflect |
| `DESTROY` | 3 | deactivate sprite | deactivate sprite | deactivate when fully outside |
| `STOP` | 4 | snap to boundary, zero contact-axis velocity | zero contact-axis velocity | clamp + zero |

DETECT is the "custom response" mode: the engine reports the contact (event
carries the side; `TILEMAP(SPRITE VX/VY)` still holds the live velocity) and
BASIC writes whatever response it wants — e.g. pico_blocks' paddle applies a
position-dependent angle plus paddle-velocity kick. BOUNCE covers the
mechanical cases (ball vs bricks and walls) with zero BASIC per frame.

## Commands

All additions are subcommands of the existing `TILEMAP` token; no new command
tokens are required (`cmd_tilemap`/`fun_tilemap` dispatch by subcommand
string).

### `TILEMAP GAME fps, width, height [, bgcolour]`

Initializes the engine:

- `width`, `height` declare the game's **logical resolution** — the
  coordinate space the entire program lives in (see Logical resolution and
  scaling). The engine maps it to the physical display or refuses to start.
- `FASTGFX CREATE` at the logical size (errors if a FRAMEBUFFER is active,
  or on a port with no display). Every display-bearing port has a FASTGFX
  driver: SPI-LCD diff/DMA, scanout memcpy (vga/hdmi/dvi), esp32, host,
  pc386.
- Installs scene defaults: every created tilemap draws in slot order (1 =
  back, 4 = front), parallax 1.0, transparent colour 0 on layers 2–4; the
  sprite batch draws after the last layer; camera fixed at (0,0); background
  cleared to `bgcolour` (RGB121 index, default 0) each frame.
- `fps = 0` disables pacing (free-run); dt integration still applies.

`TILEMAP CLOSE` also tears down the engine and the FASTGFX session it opened.
Engine + tilemap teardown additionally runs at program end and `NEW` via a
cleanup hook — this hook is new (today `tilemap_closeall` is reachable only
from `TILEMAP CLOSE`, and `bc_fastgfx_reset` only runs around FRUN), and is
the one piece of core wiring this feature adds.

### `TILEMAP LAYER slot, id [, parallax] [, transparent]`

Overrides one scene slot. `slot` 1–4 is draw order; `id` is a tilemap;
`parallax` is a float camera-scroll multiplier (0.5 = background at half
speed); `transparent` is an RGB121 colour index or -1 for opaque.
`TILEMAP LAYER slot, 0` clears a slot. Note MAX_TILEMAPS is 4 and sprite
atlases occupy tilemap slots too (one per distinct tile size), so layer-heavy
games budget slots deliberately.

### `TILEMAP CAMERA id [, ox, oy]` / `TILEMAP CAMERA OFF`

Locks the viewport to sprite `id` (centred, plus optional pixel offset),
clamped to world bounds, applied each step before render. `OFF` returns to
manual `TILEMAP VIEW` control.

### Sprite physics and filtering

| Command | Effect |
|---|---|
| `TILEMAP SPRITE VEL id, vx, vy` | Velocity in **pixels/second** (floats accepted). |
| `TILEMAP SPRITE ACCEL id, ax, ay` | Acceleration in px/s². Gravity is `ACCEL id, 0, g`. |
| `TILEMAP SPRITE HITBOX id, ox, oy, w, h` | Collision box relative to the sprite cell (both demos use hitboxes smaller than the tile: 12 px ball in a 16 px cell). Default: the full cell. |
| `TILEMAP SPRITE SOLID id, mask, mode` | Tile-attribute mask that this sprite treats as solid, and the response mode. Engine semantics: mask 0 = no solid collision (note: the underlying `TILEMAP(COLLISION)` function gives mask 0 the opposite meaning — "any tile"; the engine special-cases it). Default mask 0. |
| `TILEMAP SPRITE COLL id, mode` | Sprite-pair response mode. Default DETECT. |
| `TILEMAP SPRITE GROUP id, group, collidemask` | Pair events/responses occur only when `(group(a) AND mask(b)) OR (group(b) AND mask(a))` is non-zero — the pico-gamer rule. Default group &HFF, mask &HFF; opting a sprite out of everything means setting both sides. |
| `TILEMAP SPRITE EDGE id, mode` / `TILEMAP SPRITE EDGE id, l, t, r, b` | World-edge response, one mode for all edges or per-edge (pico_blocks: bounce/bounce/bounce on left/top/right, DETECT on bottom = the miss). Default NONE. |
| `TILEMAP SPRITE EVENTTILES id, mask` | Tile-attribute mask that triggers `TILE` events on contact-enter — for specials (collectibles, damage, ladders) and for solid tiles the game reacts to (breakable bricks: attr SOLID+BRICK, solid mask blocks, event mask reports with C/R for `TILEMAP SET` removal). Default 0. |

`TILEMAP SPRITE MOVE` (existing) position-drives a sprite and **preserves
contact history** — per-frame MOVE is a first-class driving style (paddles,
formation marches) and must not re-trigger enter events for persisting
contacts. It resets the sub-pixel accumulator. `CREATE` and `DESTROY` clear
contact history. `TILEMAP SPRITE SET` animates, as today.

## Functions

### Events

- `TILEMAP(EVENTS)` — number of events produced by the last UPDATE.
- `TILEMAP(EVENT i, sel)` — field `sel` of event `i` (1-based):

| `sel` | Meaning |
|---|---|
| `T` | Type: 1 = sprite pair, 2 = tile, 3 = world edge |
| `A` | Sprite id |
| `B` | Type 1: other sprite id |
| `S` | Contact side(s) on sprite A, bitmask: 1 left, 2 top, 4 right, 8 bottom (corners set two bits — test with AND, not equality) |
| `C`, `R` | Type 2: tile column, row |
| `I` | Type 2: tile index |

- `TILEMAP(DROPPED)` — events discarded because the fixed buffer (64 entries)
  filled this step. Normally 0.

Events report contact **enters** only: a pair or tile contact present last
step does not re-report.

### Status and time

- `TILEMAP(STATUS id)` — per-sprite bitfield, valid until the next UPDATE:
  bit 0 landed (stopped downward), 1 blocked up, 2 blocked left, 3 blocked
  right, 4–7 touching world edge left/top/right/bottom. Platformer logic
  reads `landed` for jump permission rather than consuming events.
- `TILEMAP(DT)` — last step's dt in milliseconds (float), for BASIC-side
  timers and animation.
- `TILEMAP(WIDTH)` / `TILEMAP(HEIGHT)` — the logical resolution.
- `TILEMAP(SPRITE VX id)` / `TILEMAP(SPRITE VY id)` — current velocity in
  px/s (reflects engine responses: zeroed by STOP, reflected by BOUNCE,
  untouched by DETECT).
- `TILEMAP(SPRITE X/Y id)` **remain readable after a sprite is
  deactivated** (last position). A DESTROY response can retire a sprite
  before BASIC drains the event that reports it; the event handler still
  needs the position (e.g. to place an explosion). Found by the Picovaders
  rewrite.

## Physics model

- **Units**: positions in pixels, velocity px/s, acceleration px/s².
  (pico-gamer's "64 = 1 px/frame at 60 fps" with `dt*60` scaling is the same
  unit in fixed-point clothing; MMBasic takes plain floats.)
- **Representation**: positions and velocities are int32 24.8 fixed point
  (±8.3 M px covers the 10000-tile × 256 px maximum map). Sub-pixel position
  is required — at 30 px/s and 60 fps each step moves half a pixel. The
  public `TILEMAP(SPRITE X id)` remains the integer truncation. dt itself is
  microseconds, not 24.8.
- **Integration**: semi-implicit Euler (`v += a*dt` then `pos += v*dt`),
  stable for game use and cheap in fixed point.
- **dt clamp**: 100 ms. A stall slows the world down rather than teleporting
  sprites through geometry.
- **Tunneling guard**: displacement per resolve step is capped at the tile
  size, with a bounded sub-step count per UPDATE; larger moves run multiple
  internal sub-steps. Invisible to BASIC.
- **Solid resolution** is per-axis against the sprite's hitbox with
  snap-to-boundary — the standard tile-platformer scheme, matching the
  TILEMAP manual's documented BASIC pattern.

## Rendering, performance, and HUD contract

UPDATE clears the back buffer to the background colour, renders world layers
and sprites, and returns. Anything BASIC draws after UPDATE — with any
ordinary drawing command — lands on top and is presented by the next UPDATE's
swap. There is no HUD API: the contract is *draw your HUD after UPDATE*.

**The render step must use an aligned nibble-copy blit, not the per-pixel
path.** `blit121()` currently writes through `DrawPixel` (bounds check +
encode per pixel; its own header notes ~10–30× slower than a memcpy blit) —
acceptable for today's few-tiles-per-frame usage, not for full-viewport
redraw every frame. The FASTGFX back buffer and the tile atlas share the
RGB121 nibble-packed layout, so a tile row is a straight byte copy (a 16-px
row = 8 bytes; full 320×240 repaint ≈ 38 KB of memcpy, well under a
millisecond) with a nibble-shifted variant for odd x. The engine renders
through this fast path; per-pixel remains the fallback for odd alignments.

Present cost is handled per display class by the existing FASTGFX drivers —
the back-vs-front diff makes *transmission* proportional to change, which is
why the engine renders the full viewport without dirty-region bookkeeping
(draw cost is the fast blit's problem; send cost is the diff's):

| Backend | Driver | Present strategy |
|---|---|---|
| SPI LCD (pico, picocalc, web_rp2350) | `drivers/spi_lcd/spi_lcd_fastgfx.c` | core1 scanline diff, partial DMA |
| HDMI / DVI / VGA scanout | `drivers/fastgfx_minimal/fastgfx_minimal.c` | memcpy back buffer → scanout |
| ESP32-S3 | `ports/esp32_s3/main/esp32_fastgfx.c` | scanout or ILI9341 dirty-scanline |
| Host / pc386 | `ports/host_native/host_fastgfx.c` etc. | simulator copy |

Classic SPRITE (`Draw.c` spritebuff: LIFO background save/restore, SPRITE
INTERRUPT) is mutually exclusive with the engine — full-viewport repaint
invalidates saved backgrounds. `SETTICK` and other interrupts coexist fine
(handlers run between statements; UPDATE adds at most one frame of latency,
the same as a manual `FASTGFX SWAP` today).

## Determinism and testing

`TILEMAP UPDATE dt_ms` with a literal timestep produces identical results on
every run and platform: fixed-point math, no wall clock, no pacing wait.
Host tests drive N explicit ticks and assert exact sprite positions, status
bits, and event sequences. The host FASTGFX simulator supports the full
create/swap/sync path, so `GAME`/`UPDATE` runs under `host/run_tests.sh`
(host `set_fps` currently rejects 0 — needs a small change for free-run).

## Implementation notes

- **Placement**: new file `shared/gfx/tilemap_engine.c` beside `Tilemap.c`.
  `Tilemap.c` keeps the upstream-shaped command surface; the engine file owns
  GAME/UPDATE/scene/physics state and new subcommand parsing, called from
  `cmd_tilemap`/`fun_tilemap` dispatch. Shared code only — no `host_*`
  includes.
- **Build wiring**: add to every port's source list, including the
  hand-written `mmbasic_stdio` and `mmbasic_ansi` Makefiles, which do not
  inherit cmake source lists.
- **Tokens**: none added; `AllCommands.h` untouched. The one core change is
  the program-end/NEW cleanup hook (see `TILEMAP GAME`).
- **Error safety**: UPDATE's C pipeline must hold no unreleased resources at
  any point that can `error()` (longjmp); validate scene references (a
  DESTROYed tilemap behind a live layer) up front each step, as
  `sprite_cmd_draw` already does with its `tm->active` guard.
- **sprite_t additions** (~32 bytes/slot ≈ 2 KB, plus a 64-bit pair-contact
  bitset per sprite = 512 B): `int32_t fx, fy, vx, vy, ax, ay` (24.8),
  `uint16_t solid_mask, event_mask`, hitbox `int8_t ox, oy; uint8_t w, h`,
  `uint8_t group, collide_mask, status`, packed response modes (solid, pair,
  4 × edge).
- **Event buffer**: static array of 64 × 8-byte records plus drop counter.
- **Logical resolution plumbing**: GAME allocates the FASTGFX buffers at
  the logical size and swaps the drawing system's HRes/VRes to logical for
  the session (restored on CLOSE) so every draw command targets game
  space — the same mechanism the resolution-independent display init
  already relies on. The present path gains a scale/offset (2× pixel/line
  doubling in the SPI line expansion; row duplication in the scanout copy);
  this is the one FASTGFX driver-API extension the engine needs.
- **Memory budget**: engine state ≈ 3 KB. Dominant costs remain the FASTGFX
  buffers (~78 KB at 320×240 on SPI; one back buffer on scanout targets) and
  map data. On RP2040, FASTGFX + FRUN already cannot coexist on the 128 KB
  heap; engine games on RP2040 run interpreted or at reduced resolution.
- **Perf budget** (RP2350 @ 60 fps, 16.7 ms): full-viewport fast-blit render
  ≲ 1 ms; physics + all-pairs detection for 64 sprites ≲ 0.5 ms; present is
  core1's problem on SPI. The frame belongs to BASIC.

## Compatibility

All changes are additive. The upstream TILEMAP command surface (matherp's
documented API: CREATE, ATTR, SET, DRAW, SCROLL, VIEW, CLOSE, SPRITE
CREATE/MOVE/SET/DRAW/DESTROY/CLOSE, and the query functions) is unchanged,
and manual-control programs that call `TILEMAP DRAW` + `FASTGFX SWAP`
themselves continue to work without the engine.

## Validation targets

Rewrites of the in-tree demos are the acceptance tests for the API:

- **pico_blocks** — ball: solid-mask BOUNCE vs border tiles, EVENTTILES on
  bricks (event C/R drives `TILEMAP SET` removal), per-edge
  bounce/bounce/bounce/DETECT for the bottom miss, paddle pair contact in
  DETECT mode with the angle kick in BASIC, paddle EDGE clamp. Exercises
  hitboxes (12 px ball), px/sec (kills the in-tree per-FPS recalibration).
- **A platformer smoke test** (new, no in-tree precedent): gravity via ACCEL,
  STOP solids, `landed` status for jump, ladder/coin event tiles.
- **Picovaders** — rewritten as `demos/apps/picovaders_engine.bas` with 4
  alien rows (44 aliens; 58 sprites peak, inside the 64 cap). Exercises
  pair events with DESTROY responses (bullet/alien/UFO/bomb/player), tile
  events chipping bunker tiles via `TILEMAP SET`, position-driven MOVE
  march (history-preserving), `UPDATE 0` freeze-frames for every explosion,
  and engine-velocity shots. Source of the read-after-destroy position
  guarantee above.

## Open questions / later

- Sprite-pair physical push-out (moving platforms you stand on): pairs apply
  response modes to velocity only in v1.
- Animated tiles (pico-gamer's `TILE_PROP ANIM`: tile N alternates with N+1
  on a fixed period) — cheap and attractive; needs an attribute bit and a
  frame counter. Likely v1.1.
- Sprite rotation (pico-gamer rotates vector sprites; RGB121 raster rotation
  exists in classic SPRITE) — out of scope for v1.
- Engine-side animation sequences (auto-advancing `SPRITE SET`) — BASIC via
  `TILEMAP(DT)` first; revisit if per-frame BASIC cost shows up.
- MAX_SPRITES 64 / MAX_TILEMAPS 4 headroom — revisit when a real game hits
  the cap (5-row Picovaders would).
- Adaptive resolution (`TILEMAP GAME 60, 0, 0` = logical equals native,
  game lays itself out from `TILEMAP(WIDTH/HEIGHT)`) — escape hatch for
  programs that want to genuinely use every display shape; deferred until
  someone writes one.
- Scale factors above 2× (a 160×120 game on 640×480 could be 4×) — capped
  at 2× for now; revisit if tiny logical resolutions become a real style.
