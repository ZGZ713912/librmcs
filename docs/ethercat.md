# EVK EtherCAT host transport

The Linux C++ SDK supports one HPM5E31 EVK with one classic CAN channel.
USB board APIs remain available. UART, GPIO, IMU and CAN FD are not exposed by
`librmcs::board::EvkEcat`.

## Build

```sh
cmake -S host -B host/build-ethercat -GNinja \
  -DLIBRMCS_ENABLE_ETHERCAT=ON -DCMAKE_BUILD_TYPE=Release
cmake --build host/build-ethercat
```

SOEM 1.4 is pinned to commit `abbf0d42e38d6cfbaa4c1e9e8e07ace651c386fd`.
For offline builds add
`-DFETCHCONTENT_SOURCE_DIR_LIBRMCS_SOEM=/absolute/path/to/SOEM`.
Retain upstream source/license and review SOEM licensing before distribution.
When linking the static SDK manually, also link `librmcs_soem_backend`, libusb,
pthread and rt. CMake source consumers get the dependency through the SDK target.

The default build leaves EtherCAT disabled and does not fetch SOEM. Constructing
an EtherCAT board in that build throws an explanatory exception.

## Public API

Include `<librmcs/board/evk_ecat.hpp>`. Derive a callback object from
`librmcs::board::EvkEcat::Callback`, overriding
`can_receive_callback(const librmcs::data::CanDataView&)`.
Construct `EvkEcat` with `EthercatOptions{.interface = "YOUR_DEDICATED_INTERFACE"}`
and the callback by reference.

`board.start_transmit().can_transmit(view)` submits a classic CAN request.
Keep a builder alive to batch several frames into one protocol transfer; its
destructor commits the packet. Views must contain valid standard/extended IDs,
at most eight bytes and no data bytes for a remote request.

Callbacks are serialized on a separate dispatch thread, not the PDO thread.
Their data spans are borrowed and expire when the callback returns. Copy data
before deferring work. Callback storage must outlive the board and must tolerate
calls during board construction. Do not destroy the board from a callback.

`connected()` reports the session and latched transport health, not motor feedback
freshness or successful CAN delivery. On fault, stop control, destroy the board,
then explicitly create a new connection/session. There is no automatic command
replay or rearming.

## Timing and safety

- The default exchange period is 1000 us, driven by Linux timerfd/poll events,
  not an execute-then-sleep loop. This is not a hard-real-time guarantee.
- The current firmware wire format is retained: 1039 bytes per direction,
  independent stop-and-wait sequences, epoch validation and duplicate suppression.
  PDO exchange frequency is not the rate of new acknowledged control commands.
  A separate cyclic command format remains future firmware work.
- All SOEM calls belong to one master context/worker after initialization.
  Do not run another master on the same network interface.
- Transmit and callback queues are bounded to 32 protocol packets. Transmit
  overflow latches a fault; callback queue pressure withholds the receive ACK.
- The 20-ms default admission deadline includes time queued before transmission.
  WKC loss, admission timeout or a timer backlog over 10 ms stops exchanges and
  requests INIT. Delivery of the last command can be ambiguous; never replay it
  automatically. The worker does not catch up by bursting missed timer ticks.
- A 250-ms session keepalive thread is reused from the existing handler. Session
  refresh failure marks an EtherCAT session disconnected instead of terminating
  the process; legacy USB behavior is unchanged.
- Linux normally needs NET_RAW and NET_ADMIN. Production use must not depend on
  the temporary ioctl shim used in earlier container bench measurements.
- Closing a connection cannot retract CAN frames already transmitted or force a
  motor to stop. Motor-side timeouts and a physical emergency stop remain required.

No board flashing or motor operation is part of the host implementation. Actual
CAN forwarding, disconnect/restart acceptance, long-duration timing, and 1-kHz
fresh-command qualification must be performed separately on secured hardware.
