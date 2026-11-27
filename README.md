# RetailFrameGen

Frame generation for retail PS5 games you own, on a jailbroken PS5.

RetailFrameGen is a payload that attaches to a running retail game, watches the frames it hands to
the console's display output (`libSceVideoOut`), and is meant to show a generated frame between
each two of the game's: a 30 fps game shown at 60. It is separate from
[PC5](https://github.com/bornaradusin/PC5), whose frame generation only works for Windows games
running through its own Vulkan driver.

> **Status: step 1, logging only.** The payload doesn't generate any frames yet. It logs how a
> game shows its frames, so the next steps can be built on what games really do.

Use it only with games you own. It doesn't remove or get around any protection.

## The plan

1. **Watch the frame hand-off (now).** Attach to the game and log its calls to
   `sceVideoOutRegisterBuffers` (the display buffers: size, pixel format, tiling) and
   `sceVideoOutSubmitFlip` / `sceVideoOutSetFlipRate` (when and how often frames are shown).
   A game that registers buffers but never calls `sceVideoOutSubmitFlip` flips from the GPU's
   command buffer instead, and the log shows that too.
2. **Run the interpolator inside a game.** Run FSR3's optical flow and frame interpolation (the
   interpolator PC5 uses, from [ps5-framegen](https://github.com/Swordpdf/ps5-framegen)) on two
   of the game's display buffers, inside the game's process, next to Sony's graphics library.
   This is the step that decides whether the project can work.
3. **Pace the output.** Show the game's frames at twice the flip rate, with a generated frame
   before each one, the way PC5's `pw_framegen` does with a Vulkan swapchain.
4. **Per-game settings.** Turn it on per game, with fixes where a game needs them.

## Limits to expect

- **The base frame rate matters.** Interpolating from 30 fps, or less, gives ghosting and adds
  input lag. Motion looks smoother, but the controls don't get any more responsive.
- **The HUD warps.** Retail games hand over one finished picture with the HUD already in it, so
  text and menus are interpolated along with the scene.
- **Memory and GPU time.** Retail games use nearly all of both. The optical flow needs a few
  hundred MB at 4K and a few milliseconds per frame, which a game already at its limit may not
  have to spare.
- **Buffer formats.** Many games show 10-bit HDR or compressed (tiled) buffers. The interpolator
  takes 8-bit RGB today.
- **Each game is different.** Expect games that need their own fixes, and games it can't work with.

## Building

Built with [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk):

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make
```

This makes `retailframegen.elf`.

## Using it (step 1)

1. Send `retailframegen.elf` to the console's ELF loader (port 9021), for example
   `nc -q0 <console's IP> 9021 < retailframegen.elf`.
2. Within two minutes, start the game. The payload waits for it (`eboot.bin`), or uses the
   process ID in `/data/retailframegen/pid.txt` if that file exists, and attaches as soon as the
   game's display library is loaded, so the display buffers the game registers as it starts are
   logged too.
3. It logs for 30 seconds, then lets the game go on as before. The game slows down a little
   while it logs.
4. The log is `/data/retailframegen/log.txt`. Attach it to an issue together with the game's name.

## Licence

GPL-3.0-or-later (see [LICENSE](LICENSE)), as the ps5-framegen interpolator and MihawkGen it
builds on are.
