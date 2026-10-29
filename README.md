## Distributed File Sharing System (P2P)

This project is a **peer-to-peer (P2P) file sharing system** implemented in **C++**.
It allows users to form groups, share files, and download files directly from other peers. The system uses **two redundant tracker servers** to ensure reliability, and clients automatically fail over if the tracker they are using becomes unavailable.

This implementation is designed with the constraint of using only **standard system calls and libraries** (no high-level file or networking libraries).

---

##  How It Works

```
                 sync (port + 100)
   tracker 1  <------------------->  tracker 2
       ^                                 ^
       | commands                        | (failover)
       |                                 |
   client A  <--- have / get_piece ---  client C  ---> client B
   (seeder)  ---- bitfield / bytes -->  (downloading, and already serving
                                         the pieces it has to B)
```

* **Trackers** keep the metadata: users, sessions, groups, join requests, and for every shared file its size, SHA1 hashes and the list of peers seeding it. They never see file contents. Every change one tracker makes is forwarded to the other, so either can serve any client. Tracker 1 keeps redialling tracker 2 every second; each time they connect, both send their entire state, then stream later changes. A tracker that restarts empty therefore catches up as soon as the link is back.
* **Clients** split a file into **512 KB pieces** and hash each one. Uploading registers the hashes with the tracker; the file itself stays on the uploader's disk. Every client runs a small seeder server that hands out pieces of the files it shares.
* **Downloading** asks the tracker for the piece hashes and the seeders, then asks each seeder which pieces it has (`have`, one character per piece). Eight worker threads fetch pieces in parallel, at most two from any one seeder.
  * **Rarest first:** each worker takes the missing piece held by the fewest seeders, with random tie-breaks so that simultaneous downloaders want different pieces.
  * **Verification:** each piece's SHA1 is checked before it is written, and the whole file's SHA1 at the end.
  * **Bad seeders:** a seeder that can't be reached, hangs up, times out (10 s) or sends a bad piece is dropped.
  * **Refreshing:** once a second, and whenever the workers run out of work, the downloader re-asks the tracker for seeders and the seeders for their pieces. If three refreshes in a row find nothing to fetch, the download is marked failed.
* **Partial sharing:** a client registers as a seeder as soon as its download starts and serves the pieces it has already verified. A crowd of downloaders therefore feeds itself instead of everyone waiting on the original uploader (see the benchmark below).
* **Failover**: if a command to the tracker fails, the client connects to the other tracker, logs back in with the saved credentials and retries the command once.

### Wire protocol

Every message on every connection (client-tracker, tracker-tracker, peer-peer) is a **4-byte big-endian length followed by that many bytes**. TCP is a byte stream, so without this two messages sent back to back can arrive as one and a long one can arrive in pieces. The payload is a space-separated command such as `upload_file <group> <name> <size> <file_sha1> <piece_sha1>...`. Inside a field, `%`, spaces and control characters are percent-encoded (`my%20notes.txt`), so names may contain spaces. A piece reply is the raw piece bytes (empty if the peer doesn't have it), and a `have` reply is a string such as `1101`. Messages are capped at 64 MiB.

### Concurrency

The tracker runs one thread per client connection plus one for the sync link. Its shared state is guarded by several mutexes, which are always taken in one fixed order (documented in `tracker/tracker.h`) so that two threads can't deadlock.
- **Snapshots:** the state snapshot is built with every lock held, and the sync socket is claimed before those locks are released, so each change is either in the snapshot or sent after it.
- **No mutual blocking:** a reader thread is already draining the link before the snapshot is sent, so two trackers sending large snapshots to each other can't block one another. On the client, the prompt and every download thread share one tracker connection; each request and its reply happen under one lock so replies can't be swapped. The tests run clean under ThreadSanitizer (`make tsan`).

---

##  Dependencies

To compile and run this project, ensure you have:

* A **C++ compiler** with C++17 support (e.g., `g++`).
* `make` build utility.
* **OpenSSL development libraries** (for SHA1 hashing).
* **Python 3** (only to run the tests).

Install OpenSSL with your package manager:

* **Debian/Ubuntu**:

  ```bash
  sudo apt-get install libssl-dev
  ```
* **Fedora/CentOS**:

  ```bash
  sudo dnf install openssl-devel
  ```
* **macOS (Homebrew)**:

  ```bash
  brew install openssl
  ```

The code uses POSIX sockets, so it builds on Linux and macOS (on Windows, use WSL).

---

##  Directory Structure

```
.
├── common/
│   ├── utils.cpp           # message framing, SHA1, parsing, logging
│   └── utils.h
├── client/
│   ├── client.cpp
│   ├── client.h
│   ├── tracker_info.txt    # example tracker list
│   └── Makefile
├── tracker/
│   ├── tracker.cpp
│   ├── tracker.h
│   ├── tracker_info.txt    # example tracker list
│   └── Makefile
├── tests/
│   ├── integration_test.py # end-to-end tests
│   └── benchmark.py        # download benchmarks
├── benchmarks/
│   └── RESULTS.md          # latest benchmark output
├── Makefile                # builds both programs
└── README.md
```

---

##  Compilation Instructions

From the repository root:

```bash
make            # builds tracker/tracker and client/client
make test       # end-to-end tests
make tsan       # the same tests with ThreadSanitizer builds (then `make clean`)
make bench      # download benchmarks; writes benchmarks/RESULTS.md (several minutes)
```

You can still build one program on its own with `make -C tracker` or `make -C client`.

---

##  Execution Instructions

You will need at least **three separate terminals**.

### 1. The Tracker Configuration File

Both programs take a file listing the two trackers, one `ip:port` per line. An example is included as `tracker/tracker_info.txt` (and the same file in `client/`):

```
127.0.0.1:4500
127.0.0.1:4501
```

Each tracker also listens on its **port + 100** (here 4600 and 4601) for the other tracker's sync connection.

---

### 2. Start the Tracker Servers

Open **two terminals**:

**Terminal 1 (Start Tracker 1):**

```bash
./tracker/tracker tracker/tracker_info.txt 1
```

**Terminal 2 (Start Tracker 2):**

```bash
./tracker/tracker tracker/tracker_info.txt 2
```

They can be started in either order, and either one can be restarted at any time: tracker 1 retries the connection every second, and on reconnect each sends the other its full state.

---

### 3. Start a Client

Open another terminal for each client:

```bash
./client/client client/tracker_info.txt
```

The client connects to the first tracker in the file, or to the second one if the first is down.

---

### 4. Available Commands

Once inside the client, you can run:

* **User Management**

  ```
  create_user <user_id> <password>
  login <user_id> <password>
  logout
  ```

* **Group Management**

  ```
  create_group <group_id>
  join_group <group_id>
  leave_group <group_id>
  list_groups
  list_requests <group_id>          # group owner only
  accept_request <group_id> <user_id>   # group owner only
  ```

* **File Sharing**

  ```
  upload_file <group_id> <file_path>
  list_files <group_id>             # group members only
  download_file <group_id> <file_name> <destination_path>
  show_downloads                    # [D] downloading (pieces done), [C] complete, [F] failed
  stop_share <group_id> <file_name>
  ```

Names containing spaces go in double quotes, for example `download_file "my group" "holiday photos.zip" "/tmp/holiday photos.zip"`.

Downloads run in the background, so the prompt stays usable while they run.

The client also takes two optional flags, used by the tests and the benchmark:

```bash
./client/client client/tracker_info.txt --upload-limit 2048    # serve peers at most 2048 KB/s
./client/client client/tracker_info.txt --no-partial-sharing   # seed only after a download completes
```

---

### 5. Shutting Down

Type `quit` in a client or tracker terminal. Closing a client logs its user out, so its files stop being offered to other peers.

---

##  Benchmarks

`make bench` runs every peer on one machine with uploads capped at 2 MB/s each, to emulate home connections. Over plain loopback, a single seeder would be fast enough to hide everything. Full results, including every run, are in [benchmarks/RESULTS.md](benchmarks/RESULTS.md).

**Seeder scaling**: one client downloads a 16 MB file that N peers already have.

| Seeders | Time | Throughput | Speed-up |
|---:|---:|---:|---:|
| 1 | 8.20 s | 2.0 MB/s | 1.0x |
| 2 | 4.20 s | 3.8 MB/s | 2.0x |
| 4 | 2.20 s | 7.3 MB/s | 3.7x |
| 8 | 1.25 s | 12.8 MB/s | 6.6x |

**Flash crowd**: 8 clients start downloading an 8 MB file at the same moment, and only one peer has it.

| Partial sharing | Time until all 8 finish |
|---|---:|
| off (seed only after finishing) | 32.1 s |
| on | 12.0 s (2.7x faster) |

Without partial sharing the single seed must upload 8 x 8 MB, so 32 s is the floor, and the result sits exactly on it. With partial sharing, the downloaders upload to each other while they download.

---

##  Known Limitations

* **Tracker state lives in memory.** A restarted tracker gets everything back from the other one, but if both go down together, the state is lost.
* **Merging after a split is a union.** If the trackers are cut off from each other while both keep running, merging on reconnect keeps everything either side has. A deletion made during the split (a logout, a user leaving a group) can come back.
* **A restarted tracker serves clients immediately**, possibly before the other tracker's snapshot has arrived.
* **Passwords are stored and sent in plain text**, and a peer serves any piece it has to anyone who asks, without checking group membership.
* **Each piece request opens a new TCP connection**, and there is no choking or tit-for-tat incentive as in BitTorrent.
* A command retried after failover may already have been applied by the tracker that died (for example, `create_group` then reports that the group exists).
