# Carpet Defender
### A Space Invaders variant — C17 + SDL2, identical native and WASM builds

---

## Quick start

### Native (Linux/macOS)
```bash
sudo apt install libsdl2-dev    # or: brew install sdl2
make
./housekeeper
```

### WASM
```bash
# Install Emscripten first: https://emscripten.org/docs/getting_started/
make wasm
make serve          # starts python3 -m http.server 8000
# open http://localhost:8000 in browser
```

---

## Controls
| Key | Action |
|-----|--------|
| `← →` or `A D` | Move housekeeper |
| `Space` | Fire cleaning spray (one shot on screen at a time) |
| `R` or `Enter` | Restart after game over |

---

## Rules
- **Win**: Destroy all 40 invaders (20 martini glasses + 20 burgers).
- **Lose**: Carpet accumulates too many splats (≥30), an invader reaches
  the carpet, or the player is hit too many times (3 lives).
- Barricades (mid-century credenzas) absorb enemy drops and player shots.
  They degrade visually through 4 damage states.

## Scoring
| Invader | Points |
|---------|--------|
| Martini glass (top 2 rows) | 20 |
| Burger (bottom 2 rows) | 10 |

---

## Architecture / WASM compatibility notes

The single design constraint that makes native and WASM identical is that
**all state lives in a single global `G` struct** and **the game loop is a
no-argument `main_loop()` function**. Emscripten requires exactly this:

```c
// native
while (g.running) { main_loop(); SDL_Delay(16 - elapsed); }

// WASM — browser drives the loop via requestAnimationFrame
emscripten_set_main_loop(main_loop, 0, 1);
```

The only `#ifdef __EMSCRIPTEN__` in the codebase is in `main()` to switch
between these two paths. Everything else — rendering, input, physics —
compiles identically.

No external assets. The bitmap font (5×7, C designated initializers),
all sprite geometry, and all colors are embedded directly in the source.
This keeps the WASM build a single `emcc` invocation with no `--preload-file`.

---

## Extending the game

**Explosion animation on invader kill** — add an `Explosion` array to `G`,
draw decaying colored circles for a few frames.

**UFO / mystery ship** — a Bloody Mary sliding across the top row, worth
bonus points.

**Level progression** — after clearing the wave, call `init_game()` but
preserve `g.score`, increase `BASE_MS` speed, or add more rows.

**Sound (native)** — add `SDL_mixer` and a single `#ifdef __EMSCRIPTEN__`
block to use the Web Audio API via `EM_ASM`.  Or link `SDL_mixer` in both
builds (Emscripten has a `-s USE_SDL_MIXER=2` port).

**High score persistence (WASM)** — use `EM_ASM` to call
`localStorage.setItem("hk_hiscore", score)` after game over.

**Custom Emscripten shell** — create `shell.html` to embed the canvas in
your own page layout, then use `make wasm-custom`.

---

## File layout
```
housekeeper/
  housekeeper.c   – entire game (~630 lines)
  Makefile        – native + WASM targets
  README.md       – this file
```
