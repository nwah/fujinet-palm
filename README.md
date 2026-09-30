# FujiNet for Palm OS

<h2> ⚠️ WARNING: still very early WIP; anything here subject to change ⚠️</h2>

FujiNet support for Palm OS 3.x handhelds, developed on a Handspring Visor
Deluxe (Palm OS 3.1H, DragonBall EZ). The Palm talks FujiBus (the SLIP-framed
FujiNet protocol, see [docs/protocol.md](docs/protocol.md)) over a serial
link. That link can be:

- **The Visor's USB cradle**, through a bridge on a Mac (`tools/visorbridge.js`)
  to fujinet-pc. This is the everyday development setup.
- **An emulator** (CloudpilotEmu) whose serial port is bridged to fujinet-pc.
- **Real FujiNet hardware:** an ESP32-S3 acting as USB host for the Visor
  cradle (firmware build `fujiversal-palm`, see
  [Real FujiNet hardware](#real-fujinet-hardware-esp32-s3)).

## What's here

| Path | What |
|---|---|
| `core/` | Portable C FujiBus client without globals, used by the NetLib shim and `host/`. fujinet-lib's `bus/palmos` carries a copy of its packet/SLIP layer: keep them in sync |
| `host/` | `fnhost`, a Mac command-line FujiNet client, plus unit tests for `core/` |
| `palm/transport_ser.c` | Palm Serial Manager transport ("USB Library", "BuiltIn SerLib", "Serial Library") for the NetLib shim; fujinet-lib carries a copy |
| `palm/apps/fujiconfig` | **FujiConfig**: connect, WiFi, host slots, browse hosts, install or run `.prc`/`.pdb` files |
| `palm/apps/fnlink` | Link test: raw echo through fujinet-lib's serial transport, then a FujiNet query |
| `palm/apps/mastodon`, `news`, `weather`, `isstracker` | Network apps |
| `palm/apps/nettest` | Installs/removes the NetLib shim and tests it |
| `palm/apps/hello` | Minimal app, handy for install tests |
| `palm/netshim` | `fnnetlib.prc`: a NetLib replacement so existing Palm network apps (e.g. Mocha Telnet) work over FujiNet |
| `tools/` | USB bridge, HotSync installer, emulator driver, device probes |
| `run/` | fujinet-pc config and SD folder for local testing |
| `docker/` | The Palm OS toolchain (prc-tools-remix, PilRC, Palm OS SDK) as a Docker image |

Fuji Battleship's Palm version lives in the multi-platform game repo (a
`palmos` platform in `fujinet-battleship`), built with the same toolchain.

## Prerequisites

Other checkouts are expected next to this repo, wherever that is. Every
path below is relative to the repo, and each has an override:

```
fujinet-lib-palmos/     fujinet-lib, palmos branch (FNLIB=)
fujinet-firmware/       fujinet-pc is built here (FUJINET_BIN=)
cloudpilot-emu/         emulator, optional (CLOUDPILOT=)
palm-emu/               emulator session image, optional (EMU_IMAGE=)
fujinet-palm/           this repo
```

- **Docker.** The Palm toolchain is amd64-only and runs under emulation on
  Apple Silicon. `docker/palm-build.sh` builds the image on first use.
- **Node.js**, for the USB bridge and HotSync installer. `tools/install-prc.sh`
  clones and builds [palm-sync](https://github.com/jichu4n/palm-sync) into
  `tools/palm-sync` the first time it runs.
- **fujinet-lib with the Palm OS platform**, the `palmos` branch of
  [nwah/fujinet-lib-experimental](https://github.com/nwah/fujinet-lib-experimental/tree/palmos),
  checked out next to this repo as `../fujinet-lib-palmos`. All the apps
  except Hello and NetTest link against it. Override the location with
  `FNLIB=/path`.

  ```sh
  git clone -b palmos https://github.com/nwah/fujinet-lib-experimental.git ../fujinet-lib-palmos
  ```

- **fujinet-pc**, the desktop build of
  [fujinet-firmware](https://github.com/FujiNetWIFI/fujinet-firmware), RS232
  target. It must be a FujiBus-era build (2026 or later): older builds speak
  the old DTR-framed protocol.

  ```sh
  git clone https://github.com/FujiNetWIFI/fujinet-firmware.git ../fujinet-firmware
  (cd ../fujinet-firmware && ./build.sh -p RS232)    # binary in build/dist/
  ```

  `run/run-fujinet.sh` runs `../fujinet-firmware/build/dist/fujinet`; set
  `FUJINET_BIN` to use another binary.

## Building

### Host tools and tests

```sh
make -C host test           # core/ unit tests (checksums, SLIP, wire vectors)
make -C host                # host/fnhost
make -C host fnlib-test     # fujinet-lib's common code against the host transport
```

`fnhost` talks to fujinet-pc directly, which is useful to check the server
side without a Palm:

```sh
host/fnhost ready                                  # DEVICE_READY
host/fnhost hosts                                  # host slots
host/fnhost get 'N:http://example.com/'            # HTTP GET through N1
host/fnhost -s /dev/cu.usbserial-X@115200 config   # a FujiNet on a serial port
```

### Palm apps

Everything Palm-side builds inside the toolchain container. Run
`docker/palm-build.sh <command>` from this repo, or from a sibling checkout:
it mounts the parent directory, so `../fujinet-lib-palmos` builds in place.

```sh
# 1. fujinet-lib for Palm OS (once, and after changing it)
(cd ../fujinet-lib-palmos && ../fujinet-palm/docker/palm-build.sh make palmos)

# 2. An app: palm/apps/<name>/<name>.prc
docker/palm-build.sh make -C palm/apps/fujiconfig

# All apps and the NetLib shim
for d in palm/apps/*/ palm/netshim/; do
    [ -f "$d/Makefile" ] && docker/palm-build.sh make -C "$d"
done
```

Each app is a `.prc` next to its sources, e.g. `palm/apps/weather/weather.prc`
and `palm/netshim/fnnetlib.prc`.

## Running fujinet-pc

```sh
lsof -nP -iTCP:1985 -sTCP:LISTEN || run/run-fujinet.sh
```

`run/fnconfig.ini` enables bus-over-IP (raw FujiBus over TCP) on
`localhost:1985`. That is what the USB bridge and the emulator connect to.
fujinet-pc's SD card is `run/SD/`, which git ignores. Put `.prc` files in
`run/SD/palm/` to install or run them from FujiConfig.

**fujinet-pc accepts only one bus-over-IP client at a time.** Stop the
emulator before using the Visor bridge, and vice versa. `host/fnhost` gets
"connection refused" while either is connected.

## Running on a Visor over USB

You need a Visor with its stock USB cradle plugged into the Mac.

1. **Start fujinet-pc** (see above).
2. **Install the apps.** Run the installer, then press the cradle's HotSync
   button:

   ```sh
   tools/install-prc.sh palm/apps/fujiconfig/fujiconfig.prc [more.prc ...]
   ```

   The installer pauses the bridge while it runs, so HotSync gets the
   device.
3. **Start the bridge** and leave it running:

   ```sh
   node tools/visorbridge.js            # relays to localhost:1985
   node tools/visorbridge.js --trace    # also hex-dumps both directions
   node tools/visorbridge.js --echo     # loopback link test, no FujiNet
   ```

   The Visor only appears on USB while a Palm app has its USB Library
   open. The bridge logs `Visor connected` and `Connected to fujinet` each
   time an app starts.
4. **On the Visor, open FujiConfig.** Under Settings, set **Link** to "USB
   Library" and tap **Connect**. The setting is saved, and the other
   fujinet-lib apps use the same link. FujiConfig lists the host slots.
   Browse `SD` → `palm` to **Install** an app, or **Run** it: Run installs
   it, launches it, and deletes it when you leave.

Troubleshooting:

- **The Visor times out and the bridge log shows nothing when an app
  starts:** the bridge has stopped seeing the Visor. This can happen after
  an install paused it. Restart `visorbridge.js`.
- **The bridge log shows `TCP:` errors:** another client holds fujinet-pc's
  bus-over-IP port, usually the emulator or `fnhost`.
- **"Not Rsrc DB" or fatal exceptions after an install:** list the RAM
  databases, which flags broken app databases. Run this with the bridge
  stopped, then press HotSync:

  ```sh
  node tools/palm-sync/dist/bin/cli.js run --usb tools/list-ram-dbs.js
  ```

## Running in the emulator

The emulator is CloudpilotEmu (a Palm V image), with its serial port
bridged over TCP to fujinet-pc. Scripts can drive it: install, launch, tap,
screenshot. See [docs/emulator-testing.md](docs/emulator-testing.md) for
setup, tap coordinates and gotchas.

It needs a CloudpilotEmu native build with the `--serial-tcp` option and
the `tap` command: the `fujinet` branch of `../cloudpilot-emu`,
which isn't published yet. It also needs a Palm V session image with setup
done, in `../palm-emu/palmv-base.img`.

```sh
# fujinet-pc must be listening on 1985, and nothing else connected to it
tools/emu.sh start --serial-tcp localhost:1985   # ../palm-emu/palmv-base.img
tools/emu.sh cmd "install $PWD/palm/apps/fujiconfig/fujiconfig.prc" 'launch FujiNet'
tools/emu.sh shot                 # PNG of the emulator window
tools/emu.sh cmd "tap 80 150"     # tap at Palm screen coordinates
tools/emu.sh log 40               # emulator output (add --trace-serial to start for a hex dump)
tools/emu.sh stop                 # frees fujinet-pc for the Visor bridge
```

- **Use "Serial Library":** the emulator's ROM has no USB Library, so
  choose "Serial Library" as the link in FujiConfig. fujinet-lib apps use it
  anyway when no link has been saved.
- **Nothing Handspring-specific can be tested:** the emulator doesn't
  support Visor devices.
- **`launch` takes the database name,** not the launcher label: FujiConfig
  is `FujiNet`. Quote names that contain spaces: `launch "Fuji Battleship"`.

## Real FujiNet hardware (ESP32-S3)

The Mac isn't needed with this setup: the Visor's USB cradle plugs, through
a USB-C OTG adapter, into the OTG port of the ESP32-S3 board the Fujiversal
builds use (Freenove ESP32-S3 CAM). The ESP32 is the USB host and runs
FujiNet with WiFi and an SD card. The Palm side is unchanged: Link = "USB
Library".

The firmware is the `fujiversal-palm` build, on the `feat/palm-visor-usb`
branch of fujinet-firmware. `lib/hardware/VisorChannel` is the Visor's
USB-host driver. To build and flash:

```sh
./build.sh -ys fujiversal-palm && ./build.sh -b
pio run -c platformio-generated.ini -e fujiversal-palm -t upload --upload-port /dev/cu.usbmodemXXXX
```

`build.sh -u` uses the ini's `/dev/ttyACM0`, the Linux name. The debug
console runs at 460800 baud. When an app opens the link it logs
`Visor: connected 082D:0100 ...`.

## Documentation

- [docs/protocol.md](docs/protocol.md): FujiBus framing, commands, and the
  rules a client must follow.
- [docs/emulator-testing.md](docs/emulator-testing.md): the emulator setup in
  detail.
