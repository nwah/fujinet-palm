# FujiBus over RS-232: client notes for Palm OS

This is the protocol that current `fujinet-firmware` RS-232 builds speak (FEP-004, which replaced the old DTR/SIO-style framing on 2026-01-02). Everything here was checked against the firmware source and a live fujinet-pc. The fujinet-pc run used here is `run/run-fujinet.sh`, which serves bus-over-IP on `localhost:1985`.

Authoritative source:
- `../fujinet-firmware/lib/bus/rs232/FujiBusPacket.cpp`
- `../fujinet-firmware/lib/bus/rs232/rs232.cpp`

## Link
- 115200 8N1, with no flow control and no handshake lines. The baud rate comes from the firmware config (`serial_baud`) and is read at boot.
- Each transaction is one request frame followed by exactly one reply frame. The host drives everything; nothing is unsolicited.
- Bytes outside a frame go to the modem device, which is the AT interpreter.

## Framing (SLIP)
`C0 <escaped packet> C0`. The escapes are:

| Data byte | Sent as |
|---|---|
| `C0` | `DB DC` |
| `DB` | `DB DD` |

Never send an empty frame (`C0 C0`). The firmware drops it and loses sync.

## Packet (before escaping)
| Offset | Size | Field |
|---|---|---|
| 0 | u8 | device ID |
| 1 | u8 | command (in a reply: `06` ACK or `15` NAK) |
| 2 | u16 **LE** | total length, including this 6-byte header |
| 4 | u8 | checksum, calculated with this byte set to 0 |
| 5 | u8 | descriptor |
| 6… | | more descriptors (while bit 7 is set), then params (LE), then payload |

**Descriptor bits 0-2** give the params that follow:

| Value | Params |
|---|---|
| 0 | none |
| 1-4 | that many u8 |
| 5 | one u16 |
| 6 | two u16 |
| 7 | one u32 |

**Checksum:** for each byte, `c += b; c = (c >> 8) + (c & 0xFF)`, then keep the low 8 bits.

## Replies
- An ACK carries the response payload. A NAK never carries data.
- **A malformed request gets no reply at all.** Time out, then decide whether a retry is safe. It isn't for network READ or WRITE.
- A request to an unknown device gets a NAK with device `0x00`.

## Verified wire examples
| Request | Bytes | Reply |
|---|---|---|
| Fuji DEVICE_READY | `C0 70 00 06 00 76 00 C0` | ACK plus 512 × `'A'` |
| Fuji GET_ADAPTERCONFIG | `C0 70 E8 06 00 5F 00 C0` | ACK plus 140 bytes |
| Fuji GET_ADAPTERCONFIG_EXT | `C0 70 C4 06 00 3B 00 C0` | ACK plus 240 bytes |
| N1 STATUS | `C0 71 53 06 00 CA 00 C0` | ACK plus 4 bytes (avail u16 LE, connected, error) |
| N1 READ 256 | `C0 71 52 08 00 D1 05 00 01 C0` | |
| Disk1 READ sector 0 | `C0 31 52 0A 00 94 07 00 00 00 00 C0` | |

## Firmware hazards (the firmware crashes or misbehaves)
- **Missing params crash it.** A handler reads params with `vector::at()` and C++ exceptions are disabled, so a param it expects but doesn't get reboots the ESP32. Always send every param the handler reads.
- **Never READ a network unit that is not open.** The parser pointer is null and the firmware crashes.
- **Don't READ more than STATUS reports.** A network READ larger than `bytes_available` returns filler bytes.
- **Send fixed structs at full size:**
  - host slots: 256 bytes
  - device slots: 304 bytes
  - SSIDConfig: 97 bytes
- **Disk STATUS (`0x53`) falls through to the write handler.** Don't use it.

## Devices
| ID | Device |
|---|---|
| `0x70` | Fuji control |
| `0x71`-`0x78` | Network N1-N8 |
| `0x31`-`0x38` | Disks (512-byte sectors, 0-based) |
| `0x45` | Clock |
| `0x40` | Printer |
| `0x50` | Modem (its replies are raw bytes, not frames) |

The per-command parameter tables live as comments next to each wrapper in `core/fn_fuji.c` and `core/fn_net.c`, each citing the firmware handler it matches.

## Palm-side links (Handspring AN-09, confirmed on the Visor's ROM)
Every link uses the old Serial Manager API (`SerOpen`, `SerSend`, `SerReceive`, …). Only the library name differs:

| `SysLibFind` name | Link |
|---|---|
| `"USB Library"` (`libr`/`HsUs`) | USB cradle, reaching `tools/visorbridge.js` on the Mac and then fujinet-pc |
| `"BuiltIn SerLib"` | cradle UART, which is TTL level; the serial cradle adds RS-232 levels |
| `"Serial Library"` | default; Handspring `HsPrefSet` can redirect it, e.g. to a Springboard UART |

In a serial cradle, call `HsExtKeyboardEnable(false)` before `SerOpen`, because the keyboard daemon holds the UART.
