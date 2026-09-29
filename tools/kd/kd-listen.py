#!/usr/bin/env python3
"""A minimal kernel debugger for WinDosDX: prints what the kernel prints.

The kernel's debug transport (drivers/base/kdcom) speaks the Windows KD
serial protocol and, once a debugger is attached, waits for it: every
DbgPrint needs an acknowledgment, every driver load a "continue", every
ASSERT an answer. This answers all of that automatically, so a boot with
/DEBUG /DEBUGPORT=COM1 runs through and its whole debug log is kept:

    qemu ... -serial tcp:127.0.0.1:4555,server=on,wait=off
    python tools/kd/kd-listen.py --port 4555 --log kd.log [--assert-answer i]

It logs DbgPrint text, driver loads (with --modules), exceptions reported
to the debugger (code, address, first or second chance) and prompts
(RtlAssert's "Break repeatedly, break Once, Ignore, ..." is answered with
--assert-answer, "i" by default so the boot goes on).
"""

import argparse
import socket
import struct
import sys
import time

PACKET_LEADER = b"\x30\x30\x30\x30"
CONTROL_LEADER = b"\x69\x69\x69\x69"
BREAKIN = 0x62
TRAILING = 0xAA

TYPE_STATE_MANIPULATE = 2
TYPE_DEBUG_IO = 3
TYPE_ACKNOWLEDGE = 4
TYPE_RESEND = 5
TYPE_RESET = 6
TYPE_STATE_CHANGE64 = 7

INITIAL_PACKET_ID = 0x80800000
SYNC_PACKET_ID = 0x00000800

EXCEPTION_STATE_CHANGE = 0x3030
LOAD_SYMBOLS_STATE_CHANGE = 0x3031
PRINT_STRING_API = 0x3230
GET_STRING_API = 0x3231
CONTINUE_API2 = 0x313C
DBG_CONTINUE = 0x00010002
DBG_EXCEPTION_NOT_HANDLED = 0x80010001
MANIPULATE_STATE64_SIZE = 56


class Debugger:
    def __init__(self, sock, log, answer, show_modules):
        self.sock = sock
        self.log = log
        self.answer = answer.encode("ascii")
        self.show_modules = show_modules
        self.buffer = bytearray()
        self.next_id = INITIAL_PACKET_ID
        self.pending = None           # our last data packet, until acknowledged
        self.line = ""

    # -- output --------------------------------------------------------------

    def out(self, text):
        self.log.write(text)
        self.log.flush()

    def note(self, text):
        self.out("[kd] %s\n" % text)

    # -- transport -------------------------------------------------------------

    def send_control(self, packet_type, packet_id=0):
        self.sock.sendall(CONTROL_LEADER + struct.pack("<HHII", packet_type, 0, packet_id, 0))

    def send_data(self, packet_type, payload):
        checksum = sum(payload) & 0xFFFFFFFF
        packet = PACKET_LEADER + struct.pack("<HHII", packet_type, len(payload), self.next_id, checksum) \
            + payload + bytes([TRAILING])
        self.pending = packet
        self.sock.sendall(packet)

    def read(self, count):
        while len(self.buffer) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError
            self.buffer += chunk
        data = bytes(self.buffer[:count])
        del self.buffer[:count]
        return data

    def next_packet(self):
        """(control?, type, id, payload) of the next packet; skips noise."""
        window = b""
        while True:
            byte = self.read(1)
            window = (window + byte)[-4:]
            if window in (PACKET_LEADER, CONTROL_LEADER):
                break
        control = window == CONTROL_LEADER
        packet_type, count, packet_id, checksum = struct.unpack("<HHII", self.read(12))
        if control:
            return True, packet_type, packet_id, b""
        if count > 0x1000:
            return None
        payload = self.read(count)
        if self.read(1)[0] != TRAILING or (sum(payload) & 0xFFFFFFFF) != checksum:
            self.send_control(TYPE_RESEND)
            return None
        return False, packet_type, packet_id, payload

    # -- protocol --------------------------------------------------------------

    def handle_control(self, packet_type, packet_id):
        if packet_type == TYPE_ACKNOWLEDGE and self.pending:
            self.pending = None
            self.next_id ^= 1
        elif packet_type == TYPE_RESEND and self.pending:
            self.sock.sendall(self.pending)
        elif packet_type == TYPE_RESET:
            self.next_id = INITIAL_PACKET_ID
            self.pending = None

    def handle_data(self, packet_type, packet_id, payload):
        # Acknowledge first: the kernel waits for it before anything else.
        self.send_control(TYPE_ACKNOWLEDGE, packet_id & ~SYNC_PACKET_ID)

        if packet_type == TYPE_DEBUG_IO and len(payload) >= 16:
            api, level, processor = struct.unpack_from("<IHH", payload)
            if api == PRINT_STRING_API:
                self.out(payload[16:].decode("latin-1"))
            elif api == GET_STRING_API:
                prompt = payload[16:].decode("latin-1")
                self.out(prompt)
                self.out("%s\n" % self.answer.decode())
                self.note("answered the prompt with %r" % self.answer.decode())
                reply = struct.pack("<IHHII", GET_STRING_API, level, processor, 0, len(self.answer)) + self.answer
                self.send_data(TYPE_DEBUG_IO, reply)
        elif packet_type == TYPE_STATE_CHANGE64 and len(payload) >= 32:
            new_state, level, processor, cpus = struct.unpack_from("<IHHI", payload)
            thread, pc = struct.unpack_from("<QQ", payload, 16)
            status = DBG_CONTINUE
            if new_state == EXCEPTION_STATE_CHANGE:
                code, flags, _record, address, nparams = struct.unpack_from("<IIQQI", payload, 32)
                params = struct.unpack_from("<4Q", payload, 32 + 32)
                first = struct.unpack_from("<I", payload, 32 + 152)[0] if len(payload) >= 32 + 156 else 1
                self.note("exception 0x%08x at %#x (%s chance) params %s"
                          % (code, address, "first" if first else "second",
                             " ".join("%#x" % p for p in params[:nparams])))
                # Breakpoints are the kernel's way of talking to us; anything
                # else goes back to the kernel's own handlers.
                if code != 0x80000003:
                    status = DBG_EXCEPTION_NOT_HANDLED
            elif new_state == LOAD_SYMBOLS_STATE_CHANGE:
                if self.show_modules:
                    name = payload[MANIPULATE_STATE64_SIZE + 0x60:].split(b"\0")[0]
                    self.note("load symbols at %#x %s" % (pc, name.decode("latin-1")))
            else:
                self.note("state change 0x%x at %#x" % (new_state, pc))
            reply = bytearray(MANIPULATE_STATE64_SIZE)
            struct.pack_into("<IHHII", reply, 0, CONTINUE_API2, level, processor, 0, status)
            self.send_data(TYPE_STATE_MANIPULATE, bytes(reply))
        elif packet_type == TYPE_STATE_MANIPULATE:
            pass    # replies to requests we never make
        else:
            self.note("packet type %d, %d bytes" % (packet_type, len(payload)))

    def run(self, deadline):
        # A reset puts both sides at the initial packet ids.
        self.send_control(TYPE_RESET)
        while time.time() < deadline:
            try:
                packet = self.next_packet()
            except socket.timeout:
                continue
            if packet is None:
                continue
            control, packet_type, packet_id, payload = packet
            if control:
                self.handle_control(packet_type, packet_id)
            else:
                self.handle_data(packet_type, packet_id, payload)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True, help="QEMU's -serial tcp port")
    ap.add_argument("--log", help="also write the log to this file")
    ap.add_argument("--timeout", type=float, default=1800, help="stop after this many seconds")
    ap.add_argument("--assert-answer", default="i", help="answer to kernel prompts (i = ignore)")
    ap.add_argument("--modules", action="store_true", help="log every driver load")
    args = ap.parse_args()

    start = time.time()
    while True:
        try:
            sock = socket.create_connection((args.host, args.port), timeout=5)
            break
        except OSError:
            if time.time() - start > 60:
                raise SystemExit("cannot connect to %s:%d" % (args.host, args.port))
            time.sleep(0.5)
    sock.settimeout(1.0)

    class Tee:
        def __init__(self, path):
            self.file = open(path, "w", encoding="utf-8", errors="replace") if path else None

        def write(self, text):
            sys.stdout.write(text)
            if self.file:
                self.file.write(text)

        def flush(self):
            sys.stdout.flush()
            if self.file:
                self.file.flush()

    debugger = Debugger(sock, Tee(args.log), args.assert_answer, args.modules)
    try:
        debugger.run(start + args.timeout)
    except (EOFError, ConnectionError):
        debugger.note("connection closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
