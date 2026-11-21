# EVK EtherCAT transport (in development)

EtherCAT slave firmware for HPM5E00EVK, intended as a starting point for a
librmcs EtherCAT transport. Existing Lite/Pro targets remain unchanged.
This is a development image, not production-accepted motor-control firmware.

## Source layout

- `app/src/ecat/`: process frame codec, independent stop-and-wait streams,
  session bridge, SOES object dictionary, SII and HPM ESC HAL.
- `app/src/devices.hpp`: CAN0 and UART0 peripheral forwarding.
- `app/src/main.cpp`: SOES polling and application entry point.
- `cmake/soes.cmake`: pinned SOES dependency integration.
- `tools/master.py`: synchronous Python master helper requiring `pysoem==1.1.13`.
- `tools/build.sh` and `tools/export_firmware.py`: firmware build and export.

EVK test programs, the standalone PHY diagnostic and internal peripheral loopback
mode have been removed. Runtime CoE diagnostics remain available at `0xf100`.

## Process frame version 1

Each direction uses 1039 bytes: a 16-byte header and up to 1023 protocol bytes.
Integers are little-endian; serialization does not depend on C++ struct packing.

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 1 | Version, 1 |
| 1 | 1 | Reserved, 0 |
| 2 | 2 | Payload length, 0..1023 |
| 4 | 4 | Session epoch, nonzero |
| 8 | 4 | Sequence, zero means no payload |
| 12 | 4 | Last accepted opposite-direction sequence |
| 16 | 1023 | Payload and zero padding |

Sequence numbers start at 1 and skip zero on wraparound. Pending frames are
retained until acknowledged. ACK means acceptance by receiving software, not
delivery on CAN or UART. Exactly-once peripheral delivery across MCU resets is
not guaranteed. Arbitrary PDOs must not establish a session.

SM0/1 reserve 128-byte mailboxes at 0x1000/0x1080. SM2 starts at 0x1100 and SM3
at 0x1e00; the 1039-byte triple-buffered regions do not overlap. The fixed PDO
mapping has 34 byte-aligned entries per direction, each at most 248 bits.

## Hardware and runtime

- CAN0: MCAN1/J7, 1 Mbps, classic CAN.
- UART0: UART4 PC16 TX/PC17 RX, P5 pins 8/10, default 115200 8N1.
- Debug output uses a separate UART0 peripheral.
- Session termination clears queued peripheral traffic; a 20-ms wall-clock SM2
  guard suspends peripheral output during communication outages.
- CoE `0xf100` exposes the following CAN and bridge diagnostics. Counters reset
  on MCU boot.

| Subindices | Values in order |
| --- | --- |
| 1–5 | Protocol errors, backpressure polls, rejected requests, UART overruns, CAN RX FIFO0 loss events |
| 6–8 | CAN requests queued in MCAN, CAN transmissions completed, CAN frames forwarded to EtherCAT |
| 9–11 | Last MCAN error PSR and ECR, accumulated MCAN error interrupt flags |
| 12–13 | TXFQS and CCCR at the latest failed MCAN queue submission |

UART has no physical RTS/CTS flow control. Sustained backpressure can overflow
hardware receive FIFOs. Transport ACK must not be interpreted as bus delivery.

## Build and export

The current development workaround uses GCC 16 with xPack GCC 14 RV32 `ilp32`
runtime libraries because GCC 14 crashes compiling the coroutine deserializer.
The toolchain dependencies must be installed separately.

```sh
export GNURISCV_TOOLCHAIN_PATH=/path/to/gcc16-toolchain
export EVK_ILP32_RUNTIME=/path/to/xpack-rv32-toolchain
bash firmware/evk-ecat/tools/build.sh
```

Additional CMake arguments can be passed to `build.sh`. For an existing SOES
checkout, pass `-DFETCHCONTENT_SOURCE_DIR_SOES=/absolute/path/to/SOES`.
The default build directory is `build`; override with `EVK_BUILD_DIR`.
Exported ELF, BIN and checksum manifest are written to `build/artifacts/`.

Manual export:

```sh
python3 firmware/evk-ecat/tools/export_firmware.py \
  firmware/evk-ecat/build firmware/evk-ecat/build/artifacts
```

Prefer flashing the ELF with its embedded addresses using HPMicro's OpenOCD
fork. The BIN contains boot headers and address gaps and must be programmed at
0x80000000, not 0x80003000. Retain the original flash backup and disconnect
actuators before flashing.

SOES is pinned at `6ef7b9415e936c061ad30626ca00004bdde2ac62`. Its upstream license
is GPLv2 with a linking exception; retain upstream source and license when
distributing the dependency.

## Host helper and remaining integration

`EvkMaster` supports raw protocol transfers, CAN/UART sends, UART baud
configuration, receive polling and diagnostics. Call `cycle()` continuously,
call `keepalive()` at least once per second and drain `receive()` regularly.
The bounded receive queue withholds ACK when full. WKC loss raises an exception
rather than automatically replaying potentially delivered commands.
Linux SOEM normally needs NET_RAW and NET_ADMIN.

This helper is not a real-time scheduler, automatic reconnect service or public
C++ SDK transport. A stable 1-kHz control loop has not been qualified.

Remaining work includes public host transport integration, ESI, complete channel
descriptors, watchdog integration and external CAN/UART acceptance. Physical
cable loss, complete power removal and sustained external load remain unverified.
GPIO, IMU and DBUS support are not implemented for this target.
