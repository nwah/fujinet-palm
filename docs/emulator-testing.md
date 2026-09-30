# Testing Palm apps in the emulator

These notes show how to run a Palm app in CloudpilotEmu with its serial (cradle) port bridged over TCP to fujinet-pc. You can then install, launch and tap through the app, and read the results, without a Visor in the loop. They're written so an agent can follow them.

What works:
- **Serial libraries:** the "Serial Library" and "BuiltIn SerLib" paths (the old Serial Manager).
- **The whole FujiBus stack:** it's end to end. FnLink's `Fuji` button returns READY, SSID, HOST and FN version from a live fujinet-pc.

What doesn't:
- **"USB Library":** it doesn't exist on these ROMs.
- **Handspring hardware:** Visor devices are disabled in Cloudpilot, so nothing Handspring-specific can be tested. `transport_ser.c` already skips `HsExtKeyboardEnable` when the `'hsEx'` feature is absent.

## Pieces

Paths starting `../` are checkouts next to this repo; `tools/emu.sh` finds
them there by default (override with `CLOUDPILOT` and `EMU_IMAGE`).

| Thing | Where |
|---|---|
| Emulator source (branch `fujinet`) | `../cloudpilot-emu` |
| Emulator binary | `../cloudpilot-emu/src/cloudpilot/cloudpilot-emu` |
| Driver script | `tools/emu.sh` (this repo) |
| Base session image (Palm V, OS 3.x, setup done) | `../palm-emu/palmv-base.img` |
| Stock ROMs shipped with Cloudpilot | `../cloudpilot-emu/web/embedded/public/{palmv,palmiii}.rom` |
| fujinet-pc (bus-over-IP on `localhost:1985`) | `run/run-fujinet.sh` |

The Palm V has the same Dragonball EZ UART as the IIIx, which is the intended target. The IIIx itself needs a ROM dumped from a real device; pass `--device-id PalmIIIx` with it. Don't use `palmiii.rom` for serial tests. It's the original 68328 Dragonball, a different UART.

## Build (only if the binary is missing or you changed the emulator)

```sh
cd ../cloudpilot-emu && git submodule update --init
cd src && make -j10 bin
```

`src/Makefile.local` is git-ignored and already exists on this machine. It points the build at Homebrew's readline, because macOS's libedit fails to compile `Cli.cpp`. If it's missing, copy `Makefile.local.example`. Then add `-I/opt/homebrew/opt/readline/include` to the front of `INCLUDE_EXTRA` and `-L/opt/homebrew/opt/readline/lib` to `LDFLAGS_NATIVE`.

## Emulator flags added for this

- `--serial-tcp <host:port>` connects the serial UART to a TCP server as a raw byte stream.
  - It connects when Palm OS opens the port (SerOpen), and stays connected across open/close.
  - If the server goes away, it retries once a second. It logs `serial-tcp: connected/disconnected/connect … failed` to stdout.
  - Bytes the Palm sends while disconnected are dropped.
- `--trace-serial` hex-dumps every chunk as `palm->net` / `net->palm`. It also logs baud/config, RTS/DTR and open/close.
- CLI command `tap <x> <y>` sends a pen down+up at Palm screen pixels, where (0,0) is the top-left of the LCD.

The standard CLI commands are also available; type `help` in the CLI. The useful ones are:
- `install <prc>`
- `launch <db name>`
- `save-image <file>`
- `reset-soft`

## Workflow

```sh
# 0. fujinet-pc must be listening on 1985. It's often ALREADY running; don't
#    start a second one (it fails with "bind failed: Address already in use").
lsof -nP -iTCP:1985 -sTCP:LISTEN || (cd run && ./run-fujinet.sh &)

# 1. Start from the base image, so there's no calibration or setup wizard
tools/emu.sh start --serial-tcp localhost:1985 --trace-serial

# 2. Install and launch your build
tools/emu.sh cmd "install $PWD/palm/apps/fnlink/fnlink.prc" "launch FnLink"

# 3. Drive the UI (one command per step; each waits EMU_CMD_DELAY, 1.5 s by default)
tools/emu.sh cmd "tap 129 22" "tap 22 36" "tap 24 152"   # FnLink: Serial, Open, Fuji

# 4. Look at the result
tools/emu.sh shot            # prints the PNG path; view it with the Read tool
tools/emu.sh log 40          # emulator stdout + serial trace

# 5. Done
tools/emu.sh stop
```

`start` with no image uses `../palm-emu/palmv-base.img` (or `$EMU_IMAGE`). It takes any normal cloudpilot-emu arguments after the image, such as `--device-id`, `--listen <port>` for GDB, or `--debug-app <elf>`. State goes in `$EMU_DIR`, which defaults to `$TMPDIR/palm-emu`. Only one emulator runs per `EMU_DIR`.

For a link-only test that skips FujiBus, point `--serial-tcp` at a TCP echo server and use FnLink's `Echo` button. It should report `echo 256/256 OK`.

## Finding tap coordinates

- **From the source:** the easiest source is the app's `.rcp` file. `BUTTON "Open" ID OpenButton AT (4 30 36 12)` means x=4, y=30, w=36, h=12, so tap the centre: `tap 22 36`.
- **From a screenshot:** `emu.sh shot` captures the window and resizes it to 400 px tall. On 160×160 devices (window 480×688 at scale 3, with a 28 px title bar):

```
palm_x = img_x * 1.72 / 3
palm_y = (img_y * 1.72 - 28) / 3
```

- **Silkscreen and hard buttons:** the silkscreen area is below y=160. For the hard buttons, prefer `launch <app>` over tapping.

FnLink reference (its `.rcp`):

| Control | Tap |
|---|---|
| USB / UART / Serial lib selector | `tap 28 22` / `tap 78 22` / `tap 129 22` |
| Open / Close / Echo / ×20 | `tap 22 36` / `tap 62 36` / `tap 102 36` / `tap 137 36` |
| Fuji | `tap 24 152` |

**Choose "Serial" in any app that asks for a library.** The ROM has no "USB Library".

## Gotchas

- **Never take a full-screen screenshot.** Plain `screencapture` grabs the user's whole desktop. `emu.sh shot` captures only the emulator window, by its window ID.
- **Don't feed the CLI through a FIFO.** Opening it for write blocks and deadlocks the shell. `emu.sh` uses `tail -f` on a plain file instead.
- **Taps are fire-and-forget.** Nothing tells you whether a tap landed. Taps sent before the app is on screen go to whatever is showing. After a launch or a slow operation, take a `shot` before tapping further.
- **Starting from a raw ROM** runs pen calibration and then the Setup wizard. Calibration taps must match the targets: top-left ≈ `tap 9 9`, bottom-right `tap 150 150`, centre `tap 80 59`. Setup wizard: `tap 63 152` (Next), `tap 97 152` (Done). Then `save-image` it and start from that image next time.
- **The emulator ignores SIGTERM** while in its CLI loop. `emu.sh stop` uses `kill -9`.
- **Palm OS opens the serial port briefly at boot** (9600 8N1, then closes it). So a `connected` line in the log doesn't mean your app has opened the port. Look for `config 115200 8N1` and `RTS on` after your tap.
- **Timing is not real-time.** Serial bytes arrive with no baud-rate delay, so code that is timing-sensitive on hardware may behave differently here.

## More tips (from testing Fuji Battleship, 2026-09-29)

- **App names with spaces:** quote them, as in `launch "Fuji Battleship"`. Unquoted, `launch` prints its usage.
- **No text input command.** `set-user-name <name>` sets the HotSync user name, so an app that pre-fills from it (`DlkGetSyncInfo`) can get text without typing.
- **Small fonts:** the 400 px `emu.sh shot` blurs anything under about 5 px. For a crisp LCD, capture the window at full size, which is 960×1376 on a Retina display:

  ```sh
  screencapture -x -o -l <winid> full.png
  ```

  Then take pixel `(x*6+3, 56+y*6+3)` for each Palm pixel. The LCD is 6 device pixels per Palm pixel, below a 56 px title bar.
- **Blinking:** things that blink, such as cursors and ships being placed, can be in their hidden phase when you capture. Take several captures a few hundred ms apart.
- **Scripted taps race the app:** a tap sent while the app is still busy with the previous one (sounds, animation, a network poll) may arrive before the app clears its input queue, and be lost. Wait about 2 s, or check with a shot.
