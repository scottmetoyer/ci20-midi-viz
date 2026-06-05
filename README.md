# ci20-midi-viz

A MIDI-reactive GLES2 shader visualizer for the **MIPS Creator CI20** (Ingenic JZ4780,
PowerVR SGX540). You play a USB MIDI controller; fragment shaders react and render
fullscreen over HDMI — no X server, no desktop, no kernel MIDI driver.

```
USB MIDI controller ──(usbfs)──> viz (C) ──> GLES2 fragment shader ──> HDMI (fbdev)
```

It runs straight on the bare framebuffer using the PowerVR `FLIP` window system, reads
MIDI directly from USB in userspace (the stock kernel has *no* MIDI support), and renders
into a small offscreen buffer that's upscaled to 1080p for speed.

---

## Why it's built this way (the CI20's quirks)

This board is a 2013 Debian 7 "Wheezy" machine with a vendor BSP kernel (3.0.8). A few
hard constraints shaped the design — worth knowing before you extend it:

- **Onboard audio is broken** (the codec's DMA dies after the first buffer), so this is a
  *video* project. Don't expect sound out of the 3.5mm jack.
- **The kernel has no MIDI stack** — no `snd_seq`, `snd_rawmidi`, or `snd_usb_audio`
  (built-in *or* as modules). So ALSA MIDI (`aseqdump`, `amidi`) doesn't work. We read the
  USB MIDI endpoint directly with **usbfs** ioctls instead (see `midiread.c`).
- **The PowerVR driver allows only one display owner.** The Xfce desktop (lightdm) grabs
  the GPU, so EGL fails with `0x3001` while X is running. The board must **boot to a text
  console** (see setup).
- **The GPU is modest.** A fullscreen 1080p fragment shader runs ~7 fps. Rendering at a
  lower internal resolution and upscaling gets you 30–90+ fps (the `scale` argument).

---

## Repo layout

```
viz.c                 Main app: GLES2 renderer + background USB-MIDI thread + reactive uniforms
host.c                Minimal shader runner (no MIDI) — handy for testing a shader in isolation
midiread.c            Standalone MIDI monitor — prints note/CC/pitchbend events (debugging)
build.sh              Builds all three on the board
shaders/
  reactive.frag       The MIDI-reactive demo shader (used by viz)
  plasma.frag         A plain animated shader (used by glrun/host)
include/              Khronos EGL/GLES2 headers (bundled so the board can build offline)
setup/
  99-usbmidi.rules    udev rule granting userspace access to any USB MIDI device
```

> `include/` holds the standard Khronos `EGL/`, `GLES2/`, and `KHR/` headers (from
> registry.khronos.org). They're bundled because the CI20's 2013 TLS can't fetch over
> HTTPS. The PowerVR runtime `.so`s already live on the board in `/usr/lib`.

---

## One-time board setup

You only do these once.

### 1. Boot to a text console (free the GPU from X)
```sh
sudo service lightdm stop          # stop the desktop now
sudo update-rc.d lightdm disable   # don't start it on future boots
```
The board now boots to a login prompt on HDMI with the framebuffer free.
(To get the desktop back: `sudo update-rc.d lightdm enable`.)

### 2. Allow userspace USB-MIDI access
```sh
sudo cp setup/99-usbmidi.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```
Then **unplug and replug** your MIDI controller (udev applies the new permissions on a
fresh connect). This rule matches *any* USB MIDI-class device, so all your controllers
work without editing it.

---

## Build & run (on the board)

```sh
sh build.sh
./viz shaders/reactive.frag        # play your controller — visuals react
```

`viz` arguments:
```
./viz <shader.frag> [seconds] [scale]
       shader   path to a fragment shader   (default shaders/reactive.frag)
       seconds  auto-quit after N seconds   (default 0 = run until Ctrl-C)
       scale    internal-res divisor        (default 3)
```
`scale` trades sharpness for speed: `1` = native 1080p (~7 fps), `3` = 640×360 (~37 fps,
the sweet spot), `6` = 320×180 (~90 fps). Pick the lowest number that still looks good for
your shader.

---

## Writing shaders

Shaders are plain **GLSL ES 1.0** fragment shaders (text files), compiled on the board at
runtime — so iterating is just editing a `.frag` and re-running. No recompiling the C.

Start from `shaders/reactive.frag`. Your shader gets these uniforms:

| uniform | type | meaning |
|---|---|---|
| `u_time` | `float` | seconds since start |
| `u_resolution` | `vec2` | internal render resolution in pixels |
| `u_energy` | `float` | 0..1, smoothed sum of held-note velocities |
| `u_pitch` | `float` | -1..1, pitch-bend wheel |
| `u_mod` | `float` | 0..1, mod wheel (MIDI CC 1) |
| `u_note` | `float` | 0..1, most recent note number / 127 |
| `u_cc[128]` | `float[]` | 0..1, **every** MIDI CC by number — `u_cc[74]`, `u_cc[7]`, etc. |
| `u_vel[128]` | `float[]` | 0..1, current velocity of each note (0 = off) — for per-key visuals |

`u_cc[]` and `u_vel[]` mean you almost never need to touch `viz.c`: any knob/fader your
controller sends arrives in `u_cc[<number>]`, and every key's velocity is in `u_vel[<note>]`,
already. Just index the one you want in your shader. (`u_mod`/`u_note`/`u_energy` are kept as
convenient aggregates.) Run `midiread` to discover which CC numbers your controls send.

> **Uniform budget (important on this GPU):** the SGX540 reports **64** fragment uniform
> vectors (`viz` prints this at startup). Each `float[128]` array consumes ~32. So a single
> shader can use **one** of `u_cc[128]` / `u_vel[128]` plus the scalar uniforms — **not both
> at once** (declaring both overflows the 64-vector budget and the shader fails to link).
> A whole array is allocated even if you only read one element, so pick the one you need.
> If you need both CC and note data in one shader, reduce the C side to upload a smaller
> range (e.g. the 32 CCs / 61 keys you actually use) in `viz.c`.

Minimal shader skeleton:
```glsl
#ifdef GL_ES
precision mediump float;
#endif
uniform float u_time;
uniform vec2  u_resolution;
uniform float u_energy, u_pitch, u_mod, u_note;

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution.xy;   // 0..1 across the screen
    gl_FragColor = vec4(uv, 0.5 + 0.5 * sin(u_time), 1.0);
}
```

### Iterate fast on your Mac/PC first
The uniforms intentionally match the [glslViewer](https://github.com/patriciogonzalezvivo/glslViewer)
convention (`u_time`, `u_resolution`). So you can preview shaders live on a real computer
before copying them to the board:
```sh
brew install glslviewer
glslViewer shaders/reactive.frag        # u_energy/u_pitch/etc. default to 0 here
```
For the MIDI-driven uniforms, just hardcode test values while designing, then let `viz`
drive them on the board.

### Performance tips for this GPU
- Keep per-pixel math lean — every `sin`/`cos`/`pow`/`length` costs at 1080p.
- Prefer a higher `scale` (lower internal res) for heavy shaders.
- `texture2D` lookups are cheap; big loops are not.

---

## Extending the MIDI mapping

The MIDI → uniform wiring lives in `viz.c`:

- **`midi_thread()`** parses USB-MIDI packets and updates the shared `M` state
  (`M.vel[128]` per-note velocity, `M.cc[128]` controllers, `M.pitch`, `M.last_note`).
- **The render loop** snapshots `M` each frame and pushes uniforms.

To expose something new (e.g. a second knob on CC 74, or per-note data):
1. It's already captured — `M.cc[74]` is populated for any CC.
2. Add a `glGetUniformLocation(sp, "u_filter")` near the other locations.
3. Read it in the snapshot (`float filter = M.cc[74];`) and `glUniform1f(u_filter, filter);`.
4. Declare `uniform float u_filter;` in your shader and use it.

`M.vel[]` holds a velocity per note (0 = off), so per-key visuals are possible — pass the
array as a uniform array, or reduce it (centroid, count, etc.) on the CPU first.

`midiread.c` is the quickest way to see exactly what your controller sends (note numbers,
CC numbers, channels) so you know what to map. Run it, wiggle a control, read the labels.

> Note: `viz` uses the **first** USB MIDI device it finds. To target a specific one among
> several, `midiread` accepts a `VID:PID` filter (`./midiread 1c75:0288`); add the same
> selection to `viz.c`'s `find_midi()` if you need it there.

---

## Getting files onto the board

The author's setup reaches the CI20 over SSH (`ssh ci20`). Because the board runs an old
Dropbear server, **use the legacy SCP protocol** — modern SFTP-based `scp` and
`tar | ssh` bulk transfers fail against it:
```sh
scp -O localfile ci20:gles/        # -O = legacy scp protocol
```
Editing a `.frag` is small enough to also just paste over an SSH shell.

---

## How it works (architecture)

- **Display:** `eglGetDisplay(EGL_DEFAULT_DISPLAY)` + `eglCreateWindowSurface(..., (EGLNativeWindowType)0, NULL)`.
  The PowerVR `FLIP` WSEGL (configured in `/etc/powervr.ini`) renders straight to `/dev/fb0`
  fullscreen — no X, no native window needed.
- **Render-scale:** the scene shader draws into an FBO-backed texture at `screen/scale`
  resolution; a trivial textured quad upscales it to the full framebuffer. The expensive
  per-pixel work happens at the low res.
- **MIDI:** a pthread finds a USB MIDI-streaming interface in `/sys/bus/usb/devices`, opens
  its `/dev/bus/usb/BBB/DDD` node, claims the interface, and reads 4-byte USB-MIDI packets
  with `USBDEVFS_BULK` ioctls — entirely in userspace.

## Hardware notes
- SoC: Ingenic JZ4780, dual-core MIPS (mipsel), ~1 GB RAM
- GPU: PowerVR SGX540, OpenGL ES 2.0 (IMG DDK 1.11)
- OS: Debian 7 "Wheezy", kernel 3.0.8 (vendor BSP)
- Display tested at 1920×1080p60 over HDMI
