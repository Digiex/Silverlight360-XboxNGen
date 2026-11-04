"""Stops a running xenia-edge through its GDB stub (--gdbport) and prints each guest thread's registers and the
return addresses of its stack frames (the PowerPC back chain at r1, the saved link register at frame - 8, as
Microsoft's compiler and ours store it). No gdb needed: the remote protocol by hand.

    python gdbpeek.py [port] [frames]
"""
import socket
import struct
import sys


class Remote:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.buf = b''

    def _read_packet(self):
        while True:
            while b'#' not in self.buf or len(self.buf) < self.buf.index(b'#') + 3:
                chunk = self.s.recv(65536)
                if not chunk:
                    raise EOFError
                self.buf += chunk
            start = self.buf.find(b'$')
            end = self.buf.index(b'#')
            if start < 0 or start > end:
                self.buf = self.buf[end + 3:]
                continue
            data = self.buf[start + 1:end]
            self.buf = self.buf[end + 3:]
            self.s.sendall(b'+')
            return data.decode('latin-1')

    def command(self, text):
        payload = text.encode('latin-1')
        self.s.sendall(b'$%s#%02x' % (payload, sum(payload) & 0xFF))
        while True:
            reply = self._read_packet()
            if not reply.startswith('O') or reply == 'OK':
                return reply

    def interrupt(self):
        self.s.sendall(b'\x03')
        return self._read_packet()

    def reg(self, n):
        return int(self.command('p%x' % n) or '0', 16)

    def mem32(self, address):
        r = self.command('m%x,4' % address)
        return struct.unpack('>I', bytes.fromhex(r))[0] if len(r) == 8 else None


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9000
    frames = int(sys.argv[2]) if len(sys.argv) > 2 else 16
    r = Remote(port)
    # Connecting pauses the emulator (GDBStub::Listen); the stub crashes on a bare interrupt byte.
    print('stop:', r.command('?'))
    threads = []
    reply = r.command('qfThreadInfo')
    while reply.startswith('m'):
        threads += reply[1:].split(',')
        reply = r.command('qsThreadInfo')
    for t in threads:
        r.command('Hg' + t)
        pc, lr, sp = r.reg(64), r.reg(67), r.reg(1)
        gprs = ' '.join('r%d=%08x' % (i, r.reg(i)) for i in list(range(3, 13)) + [31])
        print('thread %s: pc %08x lr %08x r1 %08x\n  %s' % (t, pc, lr, sp, gprs))
        chain = []
        frame = sp
        for _ in range(frames):
            back = r.mem32(frame) if frame else None
            if not back or back <= frame:
                break
            saved = r.mem32(back - 8)
            chain.append('%08x' % (saved or 0))
            frame = back
        print('  return addresses: ' + ' '.join(chain))
    r.command('D')


if __name__ == '__main__':
    main()
