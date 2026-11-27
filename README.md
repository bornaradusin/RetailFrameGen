# RetailFrameGen

Frame generation for retail PS5 games you own, on a jailbroken PS5.

RetailFrameGen is a payload that attaches to a running retail game, watches the frames it hands to
the console's display output (`libSceVideoOut`), and is meant to show a generated frame between
each two of the game's: a 30 fps game shown at 60.

> **Status: step 1, reading only.** Attaching to a retail game with ptrace broke it (it went
> black after its intros), so the payload doesn't attach to, stop or change the game. 0.3.0
> showed the PS5's own debug memory read (`mdbg`) reads the game's real code. 0.4.0 finds the
> game's import slots for the frame functions (where the game reads their addresses from), so
> the next version can point one at a small logger inside the game.

Use it only with games you own. It doesn't remove or get around any protection.

## The plan

1. **Watch the frame hand-off (now).** Log the game's calls to the functions it can show its
   frames with: `libSceVideoOut`'s buffer registration and flips, and the AGC graphics driver's
   submits. Breakpoints through ptrace broke retail games, so this is done by first checking the
   game's code can be read (0.3.0), then putting a small logger inside the game that the game's
   own import table points to.
2. **Run the interpolator inside a game.** Run FSR3's optical flow and frame interpolation (from [ps5-framegen](https://github.com/Swordpdf/ps5-framegen)) on two
   of the game's display buffers, inside the game's process, next to Sony's graphics library.
   This is the step that decides whether the project can work.
3. **Pace the output.** Show the game's frames at twice the flip rate, with a generated frame
   before each one.
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
   process ID in `/data/retailframegen/pid.txt` if that file exists.
3. It reads the first bytes of the functions a game shows its frames with, lists the game's
   memory regions and searches its writable ones for those functions' addresses, then ends.
   This takes a few seconds. The game isn't stopped or changed, and plays as it does without
   the payload.
4. The log is `/data/retailframegen/log.txt`. Attach it to an issue together with the game's name.

## Licence

GPL-3.0-or-later (see [LICENSE](LICENSE)), as the ps5-framegen interpolator and MihawkGen it
builds on are.
