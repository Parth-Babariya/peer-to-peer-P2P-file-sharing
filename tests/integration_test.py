#!/usr/bin/env python3
"""End-to-end tests for the tracker and client binaries.

Every test starts a fresh pair of trackers on random ports, drives clients
through their stdin exactly as a user would, and checks what they print.

    make && python3 tests/integration_test.py            # all tests
    python3 tests/integration_test.py -k download        # tests whose name matches
    python3 tests/integration_test.py --wire raw         # old unframed peer protocol
"""
import argparse
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import traceback

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIECE = 512 * 1024
# A reply printed at the client prompt, as opposed to a "[log] ..." line.
OK = r"^(> )*success"
ERR = r"^(> )*error"


class Proc:
    """A child process whose stdout is collected line by line in the background."""

    def __init__(self, name, argv, cwd):
        self.name = name
        self.p = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines = []
        self.cv = threading.Condition()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        for line in self.p.stdout:
            with self.cv:
                self.lines.append(line.rstrip("\n"))
                self.cv.notify_all()

    def mark(self):
        with self.cv:
            return len(self.lines)

    def wait_for(self, pattern, since=0, timeout=10.0):
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        with self.cv:
            while True:
                for line in self.lines[since:]:
                    if rx.search(line):
                        return line
                left = deadline - time.monotonic()
                if left <= 0:
                    tail = "\n    ".join(self.lines[-8:])
                    raise AssertionError(f"{self.name}: no line matching {pattern!r} within "
                                         f"{timeout}s; last output:\n    {tail}")
                self.cv.wait(left)

    def send(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()

    def cmd(self, line, expect=OK, timeout=10.0):
        m = self.mark()
        self.send(line)
        return self.wait_for(expect, m, timeout)

    def alive(self):
        return self.p.poll() is None

    def kill(self):
        if self.alive():
            self.p.kill()
        self.p.wait()


def free_tracker_ports():
    """A base port where base, base+1 and their sync ports (+100) are all free.

    Stays below Linux's ephemeral range (32768+), where outgoing connections
    would otherwise grab ports at random."""
    while True:
        base = random.randint(20000, 32000)
        try:
            for port in [base, base + 1, base + 100, base + 101]:
                with socket.socket() as s:
                    s.bind(("127.0.0.1", port))
            return base
        except OSError:
            continue


class Cluster:
    """Two trackers plus any number of clients, all in a scratch directory."""

    def __init__(self, args):
        self.args = args
        self.dir = tempfile.mkdtemp(prefix="p2p-test-")
        base = free_tracker_ports()
        self.ports = [base, base + 1]
        self.procs = []
        with open(self.path("trackers.txt"), "w") as f:
            f.write(f"127.0.0.1:{self.ports[0]}\n127.0.0.1:{self.ports[1]}\n")
        # Same trackers, second one first: lets a client start on tracker 2.
        with open(self.path("trackers_rev.txt"), "w") as f:
            f.write(f"127.0.0.1:{self.ports[1]}\n127.0.0.1:{self.ports[0]}\n")
        self.t1 = self._spawn("tracker1", [args.tracker, "trackers.txt", "1"])
        self.t2 = self._spawn("tracker2", [args.tracker, "trackers.txt", "2"])
        self.t1.wait_for("Connected to other tracker", timeout=10)
        for t in (self.t1, self.t2):
            t.wait_for("Caught up with other tracker", timeout=10)

    def restart_tracker(self, n):
        """Kill tracker n (1 or 2) and start a fresh, empty one in its place."""
        old = self.t1 if n == 1 else self.t2
        old.kill()
        new = self._spawn(f"tracker{n}", [self.args.tracker, "trackers.txt", str(n)])
        if n == 1:
            self.t1 = new
        else:
            self.t2 = new
        # Logged once the other tracker's whole snapshot has been applied.
        new.wait_for("Caught up with other tracker", timeout=10)
        return new

    def path(self, name):
        return os.path.join(self.dir, name)

    def _spawn(self, name, argv):
        p = Proc(name, argv, self.dir)
        self.procs.append(p)
        return p

    def client(self, name, user=None, info="trackers.txt", group=None, flags=()):
        c = self._spawn(name, [self.args.client, info, *flags])
        c.seeder_port = int(re.search(r"(\d+)$", c.wait_for(r"Seeder listening on port \d+")).group(1))
        if user:
            c.cmd(f"create_user {user} pw", OK + "|already exists")
            c.cmd(f"login {user} pw")
        if group:
            c.cmd(f"create_group {group}")
        return c

    def close(self):
        for p in self.procs:
            if p.alive() and p.p.stdin:
                try:
                    p.send("quit")
                except OSError:
                    pass
        time.sleep(0.2)
        for p in self.procs:
            p.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


def write_random(path, size, seed):
    rnd = random.Random(seed)
    data = bytes(rnd.getrandbits(8) for _ in range(size)) if size < 4096 else \
        rnd.randbytes(size)
    with open(path, "wb") as f:
        f.write(data)
    return data


def join(owner, member, group):
    member.cmd(f"join_group {group}")
    owner.cmd(f"accept_request {group} {member.name}")


def frame(args, payload):
    data = payload.encode()
    return data if args.wire == "raw" else struct.pack("!I", len(data)) + data


def recv_exact(sock, n):
    """Reads exactly n bytes, or fewer if the peer closes. (MSG_WAITALL isn't
    enough: Python makes a socket with a timeout non-blocking underneath.)"""
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            break
        data += chunk
    return data


def ask(args, sock, line, binary=False):
    """Sends one message on a raw socket and returns the reply."""
    sock.sendall(frame(args, line))
    if args.wire == "raw":
        reply = sock.recv(65536)
    else:
        (n,) = struct.unpack("!I", recv_exact(sock, 4))
        reply = recv_exact(sock, n)
    return reply if binary else reply.decode(errors="replace")


def rst_close(sock):
    """Close with an immediate RST so the other side's next send() hits a dead socket."""
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    sock.close()


# ---------------------------------------------------------------- tests

def test_download_roundtrip(cl):
    a = cl.client("a", "a", group="g")
    b = cl.client("b", "b")
    join(a, b, "g")
    data = write_random(cl.path("movie.bin"), 5 * PIECE + 1234, 1)
    a.cmd(f"upload_file g {cl.path('movie.bin')}")
    b.cmd(f"download_file g movie.bin {cl.path('copy.bin')}", r"Starting download")
    b.wait_for(r"Download completed for movie.bin", timeout=20)
    with open(cl.path("copy.bin"), "rb") as f:
        assert f.read() == data, "downloaded bytes differ from the original"
    b.cmd("show_downloads", r"\[C\] \[g\] movie.bin")


def test_client_responsive_after_download(cl):
    # The finished download tells the tracker "i_am_seeder"; the next command
    # typed at the prompt must still get its own answer.
    a = cl.client("a", "a", group="g")
    b = cl.client("b", "b")
    join(a, b, "g")
    write_random(cl.path("f.bin"), PIECE + 10, 2)
    a.cmd(f"upload_file g {cl.path('f.bin')}")
    b.cmd(f"download_file g f.bin {cl.path('out.bin')}", r"Starting download")
    b.wait_for(r"Download completed for f.bin", timeout=20)
    time.sleep(0.5)
    b.cmd("list_groups", OK + " g", timeout=5)
    b.cmd("list_files g", OK + " f.bin", timeout=5)


def test_downloader_becomes_seeder(cl):
    a = cl.client("a", "a", group="g")
    b = cl.client("b", "b")
    c = cl.client("c", "c")
    join(a, b, "g")
    join(a, c, "g")
    data = write_random(cl.path("f.bin"), 3 * PIECE, 3)
    a.cmd(f"upload_file g {cl.path('f.bin')}")
    b.cmd(f"download_file g f.bin {cl.path('b.bin')}", r"Starting download")
    b.wait_for(r"Download completed", timeout=20)
    time.sleep(0.5)
    a.kill()  # only b is left to serve the file
    c.cmd(f"download_file g f.bin {cl.path('c.bin')}", r"Starting download")
    c.wait_for(r"Download completed", timeout=20)
    with open(cl.path("c.bin"), "rb") as f:
        assert f.read() == data


def test_names_with_spaces(cl):
    # Double quotes keep spaces in one word at the prompt; on the wire such a name
    # is percent-encoded, so it survives the space-separated protocol.
    a = cl.client("a", "a")
    b = cl.client("b", "b")
    a.cmd('create_group "my group"')
    join(a, b, '"my group"')
    name = "my movie 100%.bin"
    data = write_random(cl.path(name), 2 * PIECE + 5, 14)
    a.cmd(f'upload_file "my group" "{cl.path(name)}"')
    b.cmd('list_files "my group"', OK + re.escape(f' "{name}"'))
    b.cmd(f'download_file "my group" "{name}" "{cl.path("the copy.bin")}"', r"Starting download")
    b.wait_for(re.escape(f"Download completed for {name}"), timeout=20)
    with open(cl.path("the copy.bin"), "rb") as f:
        assert f.read() == data
    b.cmd("show_downloads", re.escape(f"[C] [my group] {name}"))
    a.cmd('list_files "my group', r"Unmatched")
    # The names crossed the sync link intact too.
    c = cl.client("c", "c", info="trackers_rev.txt")
    c.cmd("list_groups", OK + ' "my group"')


def test_same_name_in_two_groups(cl):
    # One client shares a different "f.bin" in each of two groups; each group's
    # downloaders must get that group's file.
    a = cl.client("a", "a")
    b = cl.client("b", "b")
    os.makedirs(cl.path("one"))
    os.makedirs(cl.path("two"))
    data = {}
    for g, d, seed in [("g1", "one", 16), ("g2", "two", 17)]:
        a.cmd(f"create_group {g}")
        join(a, b, g)
        data[g] = write_random(cl.path(f"{d}/f.bin"), PIECE + 3, seed)
        a.cmd(f"upload_file {g} {cl.path(d + '/f.bin')}")
    for g in ["g1", "g2"]:
        b.cmd(f"download_file {g} f.bin {cl.path(g + '.bin')}", r"Starting download")
    deadline = time.monotonic() + 20
    while sum("Download completed for f.bin" in l for l in b.lines) < 2:
        assert not any("Download failed" in l for l in b.lines), "a download failed"
        assert time.monotonic() < deadline, "downloads did not finish"
        time.sleep(0.1)
    for g in ["g1", "g2"]:
        with open(cl.path(g + ".bin"), "rb") as f:
            assert f.read() == data[g], f"{g} got the other group's file"
    b.cmd("show_downloads", r"\[C\] \[g2\] f.bin")


def test_sync_burst(cl):
    # Thirty commands typed in one go make tracker 1 send thirty sync messages
    # back to back; tracker 2 must apply every one of them.
    a = cl.client("a", "a")
    m = a.mark()
    for i in range(30):
        a.send(f"create_group burst{i:02d}")
    deadline = time.monotonic() + 10
    while sum("Group created" in l for l in a.lines[m:]) < 30:
        assert time.monotonic() < deadline, "not every create_group was answered"
        time.sleep(0.1)
    c = cl.client("c", "c", info="trackers_rev.txt")
    line = c.cmd("list_groups")
    missing = [f"burst{i:02d}" for i in range(30) if f"burst{i:02d}" not in line]
    assert not missing, f"tracker 2 is missing {len(missing)} of 30 groups: {missing[:5]}..."


def seeders_used(client):
    return set(re.findall(r"Downloading piece \d+ from seeder (\S+)", "\n".join(client.lines)))


def test_download_uses_several_seeders(cl):
    a = cl.client("a", "a", group="g")
    data = write_random(cl.path("f.bin"), 12 * PIECE + 99, 12)
    a.cmd(f"upload_file g {cl.path('f.bin')}")
    for name in ["b", "c"]:
        peer = cl.client(name, name)
        join(a, peer, "g")
        peer.cmd(f"download_file g f.bin {cl.path(name + '.bin')}", r"Starting download")
        peer.wait_for(r"Download completed", timeout=20)
    d = cl.client("d", "d")
    join(a, d, "g")
    d.cmd(f"download_file g f.bin {cl.path('d.bin')}", r"Starting download")
    d.wait_for(r"Download completed", timeout=20)
    with open(cl.path("d.bin"), "rb") as f:
        assert f.read() == data
    assert len(seeders_used(d)) == 3, f"pieces came from only {seeders_used(d)}"


def test_downloaders_share_pieces_before_finishing(cl):
    # The only full copy is on a slow uplink, so b and c are still downloading
    # while they trade the pieces each already has.
    a = cl.client("a", "a", group="g", flags=["--upload-limit", "2048"])
    data = write_random(cl.path("f.bin"), 16 * PIECE, 13)
    a.cmd(f"upload_file g {cl.path('f.bin')}")
    b = cl.client("b", "b")
    c = cl.client("c", "c")
    join(a, b, "g")
    join(a, c, "g")
    b.send(f"download_file g f.bin {cl.path('b.bin')}")
    c.send(f"download_file g f.bin {cl.path('c.bin')}")
    b.wait_for(r"Download completed", timeout=30)
    c.wait_for(r"Download completed", timeout=30)
    for name in ["b", "c"]:
        with open(cl.path(name + ".bin"), "rb") as f:
            assert f.read() == data, f"{name} got a corrupt copy"
    b_addr, c_addr = f"127.0.0.1:{b.seeder_port}", f"127.0.0.1:{c.seeder_port}"
    assert c_addr in seeders_used(b) or b_addr in seeders_used(c), \
        "neither downloader fetched anything from the other"


def test_cannot_log_out_someone_else(cl):
    a = cl.client("a", "a", group="g")
    b = cl.client("b", "b")
    join(a, b, "g")
    write_random(cl.path("f.bin"), PIECE, 4)
    a.cmd(f"upload_file g {cl.path('f.bin')}")
    mallory = cl.client("mallory", "mallory")
    mallory.cmd("logout a", r".")  # any answer; what matters is the effect
    b.cmd(f"download_file g f.bin {cl.path('out.bin')}", r"Starting download|" + ERR)
    b.wait_for(r"Download completed", timeout=20)


def test_passwords_are_not_stored(cl):
    # The trackers keep only a salted hash, and only the hash crosses the sync
    # link; the password itself must not show up anywhere in their output.
    a = cl.client("a")
    a.cmd("create_user alice s3cret-Passw0rd")
    a.cmd("login alice s3cret-Passw0rd")
    b = cl.client("b", info="trackers_rev.txt")  # tracker 2 knows alice only from sync
    b.cmd("login alice wrong-password", ERR)
    b.cmd("login nobody s3cret-Passw0rd", ERR)
    b.cmd("login alice s3cret-Passw0rd")
    for tracker in (cl.t1, cl.t2):
        leaked = [l for l in tracker.lines if "s3cret" in l]
        assert not leaked, f"{tracker.name} printed the password: {leaked[0]!r}"


def test_list_files_needs_membership(cl):
    a = cl.client("a", "a", group="g")
    write_random(cl.path("secret.bin"), 100, 10)
    a.cmd(f"upload_file g {cl.path('secret.bin')}")
    outsider = cl.client("outsider", "outsider")
    outsider.cmd("list_files g", ERR)
    a.cmd("list_files g", OK + " secret.bin")


def test_bad_seeder_fails_instead_of_looping(cl):
    a = cl.client("a", "a", group="g")
    b = cl.client("b", "b")
    join(a, b, "g")
    write_random(cl.path("f.bin"), 2 * PIECE, 5)
    a.cmd(f"upload_file g {cl.path('f.bin')}")
    write_random(cl.path("f.bin"), 2 * PIECE, 6)  # the only seeder now serves wrong bytes
    b.cmd(f"download_file g f.bin {cl.path('out.bin')}", r"Starting download")
    b.wait_for(r"Download failed", timeout=15)
    b.cmd("show_downloads", r"\[F\] \[g\] f.bin")


def test_seeder_survives_reset_connections(cl):
    a = cl.client("a", "a", group="g")
    write_random(cl.path("big.bin"), 8 * PIECE, 7)
    a.cmd(f"upload_file g {cl.path('big.bin')}")
    for i in range(30):
        s = socket.create_connection(("127.0.0.1", a.seeder_port))
        s.sendall(frame(cl.args, f"get_piece big.bin {i % 8}"))
        time.sleep(0.005)
        rst_close(s)
    time.sleep(0.5)
    assert a.alive(), "client process died while serving pieces"
    a.cmd("list_groups", OK + " g")


def test_seeder_survives_malformed_request(cl):
    a = cl.client("a", "a", group="g")
    write_random(cl.path("f.bin"), PIECE, 8)
    a.cmd(f"upload_file g {cl.path('f.bin')}")
    for bad in ["get_piece f.bin notanumber", "get_piece f.bin -1", "get_piece f.bin 99999999999",
                "get_piece", "\x00\x01\x02"]:
        s = socket.create_connection(("127.0.0.1", a.seeder_port))
        s.sendall(frame(cl.args, bad))
        s.settimeout(2)
        try:
            s.recv(16)
        except OSError:
            pass
        s.close()
    time.sleep(0.3)
    assert a.alive(), "client process died on a malformed peer request"
    a.cmd("list_groups", OK + " g")


def test_seeder_requires_the_share_key(cl):
    # Knowing a seeder's address and a file name is not enough: pieces go only to
    # peers with the file's share key, which the tracker gives only to members.
    a = cl.client("a", "a", group="g")
    data = write_random(cl.path("f.bin"), PIECE + 10, 15)
    a.cmd(f"upload_file g {cl.path('f.bin')}")

    def from_seeder(line, binary=False):
        with socket.create_connection(("127.0.0.1", a.seeder_port), timeout=5) as s:
            return ask(cl.args, s, line, binary)

    for line in ["have g f.bin", "get_piece g f.bin 0", f"have g f.bin {'0' * 32}", f"get_piece g f.bin {'0' * 32} 0"]:
        assert from_seeder(line) == "", f"seeder answered {line!r} without the key"

    with socket.create_connection(("127.0.0.1", cl.ports[0]), timeout=5) as t:
        ask(cl.args, t, "create_user outsider pw")
        ask(cl.args, t, "login outsider pw 5000")
        reply = ask(cl.args, t, "download_file g f.bin")
        assert reply.startswith("error"), f"a non-member got the file's metadata: {reply!r}"
        ask(cl.args, t, "join_group g")
        a.cmd("accept_request g outsider")
        reply = ask(cl.args, t, "download_file g f.bin").split(" ")
    assert reply[0] == "success", reply
    key = reply[3]
    assert from_seeder(f"have g f.bin {key}") == "11"
    assert from_seeder(f"get_piece g f.bin {key} 1", binary=True) == data[PIECE:]


def test_tracker_survives_garbage(cl):
    for blob in [b"\xff\xff\xff\xff", b"\x00\x00\x00\x05abc", b"login", b"\x00" * 64]:
        s = socket.create_connection(("127.0.0.1", cl.ports[0]))
        s.sendall(blob)
        rst_close(s)
    time.sleep(0.3)
    assert cl.t1.alive(), "tracker 1 died on garbage input"
    a = cl.client("a", "a")
    a.cmd("list_groups")


def test_tracker_rejects_malformed_upload(cl):
    # Talk to the tracker directly, as a buggy or hostile client could.
    s = socket.create_connection(("127.0.0.1", cl.ports[0]))
    s.settimeout(5)
    ask(cl.args, s, "create_user z pw")
    ask(cl.args, s, "login z pw 5000")
    ask(cl.args, s, "create_group q")
    key = "0" * 32
    reply = ask(cl.args, s, f"upload_file q f.bin notanumber deadbeef {key} h")
    assert reply.startswith("error"), f"unexpected reply {reply!r}"
    reply = ask(cl.args, s, f"upload_file q f.bin 2000000 deadbeef {key} onlyonehash")
    assert reply.startswith("error"), f"accepted 1 piece hash for a 4-piece file: {reply!r}"
    reply = ask(cl.args, s, "upload_file q f.bin 10 deadbeef guessable h")
    assert reply.startswith("error"), f"accepted a malformed share key: {reply!r}"
    s.close()
    time.sleep(0.3)
    assert cl.t1.alive(), "tracker 1 died on a malformed upload"


def test_uploads_during_synced_logouts(cl):
    # Tracker 2 handles uploads itself (groups lock, then sessions lock) while
    # applying logouts synced from tracker 1 (sessions, then groups). Opposite
    # lock orders can deadlock; run under ThreadSanitizer this shows up as a
    # lock-order inversion even when the timing doesn't hit.
    up = cl.client("up", "up", info="trackers_rev.txt", group="g")
    write_random(cl.path("f.bin"), 1000, 9)
    for i in range(10):
        churn = cl.client(f"churn{i}", f"churn{i}")
        churn.cmd("logout")
        up.cmd(f"upload_file g {cl.path('f.bin')}")
    up.cmd("list_files g", OK + " f.bin", timeout=5)
    assert cl.t2.alive()


def restarted_tracker_catches_up(cl, n):
    survivor = cl.t2 if n == 1 else cl.t1
    a = cl.client("a", "a", group="g")
    b = cl.client("b", "b")
    data = write_random(cl.path("f.bin"), 3 * PIECE + 7, 11)
    a.cmd(f"upload_file g {cl.path('f.bin')}")
    (cl.t1 if n == 1 else cl.t2).kill()
    # Changes made while tracker n is down exist only on the survivor.
    c = cl.client("c", "c", info="trackers.txt" if n == 2 else "trackers_rev.txt")
    join(a, c, "g")
    c.cmd("create_group made_while_down")
    cl.restart_tracker(n)
    # Now lose the survivor: tracker n alone must know everything.
    survivor.kill()
    b.cmd("list_groups", OK + r" .*made_while_down")  # b fails over and logs back in
    c.cmd("list_files g", OK + " f.bin")
    c.cmd(f"download_file g f.bin {cl.path('c.bin')}", r"Starting download")
    c.wait_for(r"Download completed", timeout=20)
    with open(cl.path("c.bin"), "rb") as f:
        assert f.read() == data


def test_restarted_tracker1_catches_up(cl):
    restarted_tracker_catches_up(cl, 1)


def test_restarted_tracker2_catches_up(cl):
    restarted_tracker_catches_up(cl, 2)


def test_lone_restarted_tracker_waits_before_serving(cl):
    # A restarted tracker must not answer from its empty state while the other
    # tracker's state may be on its way. With the other one down for good, it
    # turns clients away for a grace period, then serves alone.
    cl.t1.kill()
    cl.t2.kill()
    start = time.monotonic()
    t1 = cl._spawn("tracker1", [cl.args.tracker, "trackers.txt", "1"])
    t1.wait_for("Tracker listening for clients")
    with socket.create_connection(("127.0.0.1", cl.ports[0]), timeout=5) as s:
        reply = ask(cl.args, s, "list_groups")
    assert "catching up" in reply, f"answered before catching up: {reply!r}"
    # A client typing meanwhile just waits, then gets served.
    c = cl.client("c")
    c.cmd("create_user c pw", OK, timeout=15)
    assert time.monotonic() - start >= 2.5, "served before the grace period ended"
    t1.wait_for("serving clients from our own state")


def test_retried_command_runs_once(cl):
    # After a failover the client resends a command under the same request id.
    # If the tracker that died had already run it, the retry must get the first
    # reply, not "group already exists".
    s1 = socket.create_connection(("127.0.0.1", cl.ports[0]), timeout=5)
    ask(cl.args, s1, "create_user z pw")
    ask(cl.args, s1, "login z pw 5000")
    first = ask(cl.args, s1, "#0011223344556677 create_group g")
    assert first.startswith("success"), first
    s1.close()
    cl.t1.kill()
    s2 = socket.create_connection(("127.0.0.1", cl.ports[1]), timeout=5)
    ask(cl.args, s2, "login z pw 5000")
    retry = ask(cl.args, s2, "#0011223344556677 create_group g")
    assert retry == first, f"the retry ran again: {retry!r}"
    again = ask(cl.args, s2, "#8899aabbccddeeff create_group g")
    assert again.startswith("error"), f"a new request was answered from the cache: {again!r}"
    s2.close()


def split(cl):
    """Cuts the sync link (tracker 1 stops dialling) until heal()."""
    m = cl.t2.mark()
    cl.t1.send("disconnect")
    cl.t2.wait_for("Connection with other tracker lost", m)


def heal(cl):
    marks = cl.t1.mark(), cl.t2.mark()
    cl.t1.send("reconnect")
    for t, m in zip((cl.t1, cl.t2), marks):
        t.wait_for("Caught up with other tracker", m)


def test_split_does_not_undo_removals(cl):
    # While the trackers can't reach each other, b leaves the group and a stops
    # sharing a file, both on tracker 1; tracker 2 still has the older state.
    # After the merge, those removals must stand on both trackers, and a change
    # made only on tracker 2 must reach tracker 1.
    a = cl.client("a", "a", group="g")
    b = cl.client("b", "b")
    e = cl.client("e", "e")
    join(a, b, "g")
    join(a, e, "g")
    write_random(cl.path("f.bin"), 1000, 18)
    a.cmd(f"upload_file g {cl.path('f.bin')}")
    split(cl)
    b.cmd("leave_group g")
    a.cmd("stop_share g f.bin")
    c = cl.client("c", "c", info="trackers_rev.txt")
    c.cmd("create_group made_on_2")
    heal(cl)
    b.cmd("list_files g", ERR)
    b.cmd("list_groups", OK + r" .*made_on_2")
    cl.t1.kill()  # from here on, tracker 2 answers
    b.cmd("list_files g", ERR)
    e.cmd("download_file g f.bin " + cl.path("e.bin"), r"No seeders")


def test_state_survives_both_trackers_dying(cl):
    # Every change is on disk before the client hears back, so killing both
    # trackers at once (SIGKILL: no chance to save anything) loses nothing.
    a = cl.client("a", "a", group="g")
    b = cl.client("b", "b")
    join(a, b, "g")
    data = write_random(cl.path("f.bin"), 2 * PIECE + 1, 19)
    a.cmd(f"upload_file g {cl.path('f.bin')}")
    cl.t1.kill()
    cl.t2.kill()
    cl.t1 = cl._spawn("tracker1", [cl.args.tracker, "trackers.txt", "1"])
    cl.t2 = cl._spawn("tracker2", [cl.args.tracker, "trackers.txt", "2"])
    for t in (cl.t1, cl.t2):
        t.wait_for(r"Loaded [1-9]\d* changes")
        t.wait_for("Caught up with other tracker")
    b.cmd("list_files g", OK + " f.bin")  # b reconnects and logs back in
    b.cmd(f"download_file g f.bin {cl.path('b.bin')}", r"Starting download")
    b.wait_for(r"Download completed", timeout=20)
    with open(cl.path("b.bin"), "rb") as f:
        assert f.read() == data


def test_failover_keeps_session(cl):
    a = cl.client("a", "a", group="g")
    cl.t1.kill()
    a.cmd("list_groups", OK + " g", timeout=10)
    a.cmd("create_group g2")  # needs a logged-in session on tracker 2


TESTS = [v for k, v in sorted(globals().items()) if k.startswith("test_")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tracker", default=os.path.join(ROOT, "tracker", "tracker"))
    ap.add_argument("--client", default=os.path.join(ROOT, "client", "client"))
    ap.add_argument("--wire", choices=["framed", "raw"], default="framed",
                    help="peer wire format used by the raw-socket tests")
    ap.add_argument("-k", default="", help="only run tests whose name contains this")
    args = ap.parse_args()
    args.tracker, args.client = os.path.abspath(args.tracker), os.path.abspath(args.client)

    failed = 0
    tests = [t for t in TESTS if args.k in t.__name__]
    for test in tests:
        cl = Cluster(args)
        start = time.monotonic()
        try:
            test(cl)
            print(f"PASS  {test.__name__}  ({time.monotonic() - start:.1f}s)", flush=True)
        except Exception as e:
            failed += 1
            msg = str(e) if isinstance(e, AssertionError) else traceback.format_exc()
            print(f"FAIL  {test.__name__}  ({time.monotonic() - start:.1f}s)\n  {msg}", flush=True)
        finally:
            cl.close()
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    main()
