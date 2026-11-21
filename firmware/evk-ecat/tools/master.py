"""Small synchronous EVK master. Requires pysoem==1.1.13 and raw-network access.

Not a real-time scheduler or a replacement for the public librmcs C++ SDK.
"""
import secrets
import collections
import struct
import time

import pysoem


class EvkMaster:
    SIZE = 1039
    HEADER = struct.Struct("<BBHIII")

    def __init__(self, interface):
        self.master = pysoem.Master()
        self.master.open(interface)
        self.sequence = 0
        self.ack = 0
        self.epoch = secrets.randbelow(0xFFFFFFFF) + 1
        self.pending = None
        self.received = collections.deque()
        self.last_keepalive = time.monotonic()
        try:
            if self.master.config_init() != 1:
                raise RuntimeError("Expected exactly one EVK")
            self.slave = self.master.slaves[0]
            if self.slave.man != 0 or self.slave.id != 0x524D4353:
                raise RuntimeError("Unexpected EtherCAT identity")
            self.slave.state = pysoem.PREOP_STATE
            self.slave.write_state()
            if self.slave.state_check(pysoem.PREOP_STATE, 500000) != pysoem.PREOP_STATE:
                raise RuntimeError("PREOP failed")
            if self.master.config_map() != self.SIZE * 2:
                raise RuntimeError("Unexpected PDO layout")
            self.master.send_processdata()
            self.master.receive_processdata(10000)
            self.master.state = pysoem.OP_STATE
            self.master.write_state()
            if self.master.state_check(pysoem.OP_STATE, 500000) != pysoem.OP_STATE:
                raise RuntimeError("OP failed")
            self.nonce = secrets.randbits(32)
            self.send(b"\x00\x01" + struct.pack("<I", self.nonce))
            expected = b"\x00\x11" + struct.pack("<I", self.nonce)
            deadline = time.monotonic() + 2
            while expected not in self.received:
                if time.monotonic() > deadline:
                    raise TimeoutError("StartAck missing")
                self.cycle()
            self.received.remove(expected)
        except BaseException:
            self.close()
            raise

    def cycle(self):
        payload = self.pending or b""
        self.slave.output = self.HEADER.pack(
            1, 0, len(payload), self.epoch, self.sequence if self.pending else 0, self.ack
        ) + payload + bytes(1023 - len(payload))
        self.master.send_processdata()
        wkc = self.master.receive_processdata(10000)
        if wkc != self.master.expected_wkc:
            raise ConnectionError(f"EtherCAT WKC {wkc}, expected {self.master.expected_wkc}")
        data = self.slave.input
        version, reserved, length, epoch, sequence, ack = self.HEADER.unpack_from(data)
        if version != 1 or reserved or length > 1023 or epoch != self.epoch:
            return
        if self.pending is not None and ack == self.sequence:
            self.pending = None
        if sequence == 0 or sequence == self.ack:
            return
        if sequence != (self.ack % 0xFFFFFFFF) + 1:
            raise RuntimeError("Nonconsecutive uplink sequence")
        if len(self.received) >= 128:
            return  # Withhold ACK until the application drains its receive queue.
        self.received.append(data[16:16 + length])
        self.ack = sequence

    def send(self, payload, timeout=2):
        if not payload or len(payload) > 1023 or self.pending is not None:
            raise ValueError("Invalid payload or previous transfer unresolved")
        self.sequence = self.sequence % 0xFFFFFFFF + 1
        self.pending = bytes(payload)
        deadline = time.monotonic() + timeout
        while self.pending is not None:
            self.cycle()
            if time.monotonic() >= deadline:
                raise TimeoutError("Admission timeout: delivery status unknown; do not replay blindly")
            time.sleep(.001)

    def keepalive(self):
        self.send(b"\x00\x21" + struct.pack("<I", self.nonce))
        self.last_keepalive = time.monotonic()

    def send_can(self, can_id, data=b"", extended=False, remote=False):
        if len(data) > 8 or (remote and data) or not 0 <= can_id <= (0x1FFFFFFF if extended else 0x7FF):
            raise ValueError("Invalid classic CAN frame")
        flags = 2 | (0x20 if extended else 0) | (0x40 if remote else 0) | (0x80 if data else 0)
        header = can_id | ((len(data) - 1 if data else 0) << (29 if extended else 13))
        self.send(bytes([flags]) + header.to_bytes(4 if extended else 2, "little") + bytes(data))

    def send_uart(self, data, idle_delimited=True):
        if not 0 < len(data) <= 1021:
            raise ValueError("UART payload must fit one protocol transfer")
        # Use the extended-length encoding even for short messages.
        header = 11 | 0x20 | (0x10 if idle_delimited else 0) | (len(data) << 6)
        self.send(header.to_bytes(2, "little") + bytes(data))

    def configure_uart(self, baudrate):
        if not 0 < baudrate <= 0xFFFFFFFF:
            raise ValueError("Invalid baudrate")
        # Extended field ID 26 shares its last byte with the configuration payload.
        self.send(((26 << 4) | (baudrate << 12)).to_bytes(6, "little"))

    def receive(self):
        return self.received.popleft() if self.received else None

    def diagnostics(self):
        return [int.from_bytes(self.slave.sdo_read(0xF100, i), "little") for i in range(1, 6)]

    def close(self):
        try:
            self.master.state = pysoem.INIT_STATE
            self.master.write_state()
        finally:
            self.master.close()
