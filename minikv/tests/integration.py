#!/usr/bin/env python3
"""End-to-end tests for MiniKV over real TCP. Run: MINIKV=./minikv-asan python3 tests/integration.py"""
import os, signal, socket, subprocess, sys, tempfile, time

BIN = os.environ.get("MINIKV", "./minikv")
PORT = int(os.environ.get("MINIKV_PORT", "16399"))


def enc(*args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        a = a.encode() if isinstance(a, str) else a
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out


class Client:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def _fill(self):
        d = self.s.recv(1 << 20)
        if not d:
            raise EOFError("server closed connection")
        self.buf += d

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t == b"+":
            return rest.decode()
        if t == b"-":
            return Exception(rest.decode())
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            while len(self.buf) < n + 2:
                self._fill()
            data, self.buf = self.buf[:n], self.buf[n + 2:]
            return data
        if t == b"*":
            return [self.read() for _ in range(int(rest))]
        raise ValueError(line)

    def cmd(self, *a):
        self.s.sendall(enc(*a))
        return self.read()

    def close(self):
        self.s.close()


class Server:
    def __init__(self, aof, fsync="everysec"):
        self.aof, self.fsync = aof, fsync

    def start(self):
        self.p = subprocess.Popen([BIN, "-p", str(PORT), "-a", self.aof, "-f", self.fsync],
                                  stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
                return self
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def kill9(self):
        self.p.send_signal(signal.SIGKILL)
        self.p.wait()

    def stop(self):
        self.p.send_signal(signal.SIGTERM)
        err = self.p.communicate(timeout=10)[1].decode()
        assert self.p.returncode == 0, f"unclean exit {self.p.returncode}\n{err}"
        assert "ERROR: AddressSanitizer" not in err and "runtime error" not in err, err


def check(name, cond):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond:
        sys.exit(1)


def main():
    tmp = tempfile.mkdtemp()
    aof = os.path.join(tmp, "t.aof")
    srv = Server(aof, "always").start()
    c = Client()

    check("PING", c.cmd("PING") == "PONG")
    check("SET/GET", c.cmd("SET", "a", "1") == "OK" and c.cmd("GET", "a") == b"1")
    check("GET missing -> nil", c.cmd("GET", "nope") is None)
    check("INCR/DECR", c.cmd("INCR", "a") == 2 and c.cmd("DECR", "a") == 1)
    check("INCR non-integer -> error", isinstance(c.cmd("SET", "s", "abc") and c.cmd("INCR", "s"), Exception))
    check("DEL multi + EXISTS", c.cmd("DEL", "a", "s", "zzz") == 2 and c.cmd("EXISTS", "a") == 0)
    check("wrong arity -> error", isinstance(c.cmd("GET"), Exception))
    r = c.cmd("FOO\r\n+OK")
    check("unknown cmd can't inject reply", isinstance(r, Exception) and "\r" not in str(r))
    check("KEYS glob", (c.cmd("SET", "user:1", "x"), c.cmd("SET", "user:2", "y"), c.cmd("SET", "zed", "z"),
                        sorted(c.cmd("KEYS", "user:*")) == [b"user:1", b"user:2"])[-1])

    # --- pipelining: 20k commands in a single send, replies in order
    n = 20000
    c.s.sendall(b"".join(enc("SET", f"p{i}", f"v{i}") for i in range(n)) + b"".join(enc("GET", f"p{i}") for i in range(n)))
    ok = all(c.read() == "OK" for _ in range(n))
    ok = ok and all(c.read() == f"v{i}".encode() for i in range(n))
    check("pipelining 20k SET + 20k GET", ok)

    # --- fragmented input: one byte at a time
    raw = enc("SET", "frag", "bytes")
    for i in range(len(raw)):
        c.s.sendall(raw[i:i + 1])
        time.sleep(0.001)
    check("byte-by-byte fragmented command", c.read() == "OK" and c.cmd("GET", "frag") == b"bytes")

    # --- inline (telnet-style) command
    c.s.sendall(b"PING\r\n")
    check("inline command", c.read() == "PONG")

    # --- large value: forces partial send() + EPOLLOUT path and multi-read parse
    big = os.urandom(5_000_000)
    check("5MB binary-safe roundtrip", c.cmd("SET", "big", big) == "OK" and c.cmd("GET", "big") == big)

    # --- many concurrent connections
    conns = [Client() for _ in range(2000)]
    check("2000 concurrent connections", all(x.cmd("PING") == "PONG" for x in conns))
    for x in conns:
        x.close()
    check("server alive after mass disconnect", c.cmd("PING") == "PONG")

    # --- slow reader: client pipelines GETs of a big value and doesn't read yet
    c2 = Client()
    c2.s.sendall(b"".join(enc("GET", "big") for _ in range(8)))
    time.sleep(0.3)
    check("other clients unaffected by a slow reader", c.cmd("PING") == "PONG")
    check("slow reader eventually gets all data", all(c2.read() == big for _ in range(8)))
    c2.close()

    # --- expiry: lazy + active
    c.cmd("SET", "e1", "v", "PX", "150")
    c.cmd("SET", "e2", "v", "EX", "3600")
    check("TTL set", 0 < c.cmd("PTTL", "e1") <= 150 and c.cmd("TTL", "e2") > 3500)
    time.sleep(0.4)
    check("key expired (lazy)", c.cmd("GET", "e1") is None)
    for i in range(500):
        c.cmd("SET", f"x{i}", "v", "PX", "50")
    time.sleep(1.5)
    check("active expiry reclaims untouched keys", c.cmd("DBSIZE") < 20000 + 40 + 100 and c.cmd("EXISTS", "x7") == 0)
    ks = c.cmd("DBSIZE")

    # --- protocol error closes connection with an error reply
    c3 = Client()
    c3.s.sendall(b"*abc\r\n")
    check("protocol error -> -ERR then close", isinstance(c3.read(), Exception))
    c3.close()

    # --- persistence: kill -9 with fsync=always must lose nothing acknowledged
    c.cmd("SET", "counter", "0")
    for _ in range(300):
        c.cmd("INCR", "counter")
    c.cmd("EXPIRE", "e2", "7200")
    c.cmd("DEL", "p0")
    c.close()
    srv.kill9()
    srv = Server(aof, "always").start()
    c = Client()
    check("kill -9 recovery: counter", c.cmd("GET", "counter") == b"300")
    check("kill -9 recovery: 20k pipelined keys", c.cmd("GET", "p19999") == b"v19999" and c.cmd("EXISTS", "p0") == 0)
    check("kill -9 recovery: big value", c.cmd("GET", "big") == big)
    check("kill -9 recovery: TTL preserved", c.cmd("TTL", "e2") > 7000)
    check("kill -9 recovery: expired key stays gone", c.cmd("GET", "e1") is None)
    check("kill -9 recovery: key count", c.cmd("DBSIZE") <= ks + 2)

    # --- torn AOF tail (crash mid-append) is truncated, not fatal
    c.close()
    srv.stop()
    with open(aof, "ab") as f:
        f.write(b"*3\r\n$3\r\nSET\r\n$4\r\ntorn\r\n$10\r\nabc")
    srv = Server(aof, "always").start()
    c = Client()
    check("torn AOF tail tolerated", c.cmd("GET", "counter") == b"300" and c.cmd("GET", "torn") is None)
    c.cmd("SET", "after", "ok")
    c.close()
    srv.stop()
    srv = Server(aof, "everysec").start()
    c = Client()
    check("writes after truncation replay cleanly", c.cmd("GET", "after") == b"ok")
    check("INFO", b"total_commands_processed" in c.cmd("INFO"))
    c.close()
    srv.stop()
    print("\nALL INTEGRATION TESTS PASSED (server exited cleanly, no sanitizer reports)")


if __name__ == "__main__":
    main()
