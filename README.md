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
   client A  ---- get_piece i ---->  client B
   (seeder)  <--- piece bytes -----  (downloader)
```

* **Trackers** keep the metadata: users, sessions, groups, join requests, and for every shared file its size, SHA1 hashes and the list of peers seeding it. They never see file contents. Every change one tracker makes is forwarded to the other, so either can serve any client.
* **Clients** split a file into **512 KB pieces** and hash each one. Uploading registers the hashes with the tracker; the file itself stays on the uploader's disk. Every client runs a small seeder server that hands out pieces of the files it shares.
* **Downloading** asks the tracker for the piece hashes and the seeders, then fetches pieces from the seeders in turn, checking each piece's SHA1 before writing it and the whole file's SHA1 at the end. A seeder that can't be reached, hangs up, times out (10 s) or sends a piece with the wrong hash is dropped; if none are left, the download is marked failed. A finished download registers itself as a new seeder.
* **Failover**: if a command to the tracker fails, the client connects to the other tracker, logs back in with the saved credentials and retries the command once.

### Wire protocol

Every message on every connection (client-tracker, tracker-tracker, peer-peer) is a **4-byte big-endian length followed by that many bytes**. TCP is a byte stream, so without this two messages sent back to back can arrive as one and a long one can arrive in pieces. The payload is a space-separated command such as `upload_file <group> <name> <size> <file_sha1> <piece_sha1>...`; a piece reply is the raw piece bytes (empty if the peer doesn't have it). Messages are capped at 64 MiB.

### Concurrency

The tracker runs one thread per client connection plus one for the sync link. Its shared state is guarded by several mutexes, which are always taken in one fixed order (documented in `tracker/tracker.h`) so that two threads can't deadlock. On the client, the prompt and every download thread share one tracker connection; each request and its reply happen under one lock so replies can't be swapped. The tests run clean under ThreadSanitizer (`make tsan`).

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
│   └── integration_test.py # end-to-end tests
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

Start both within a couple of seconds: tracker 1 connects to tracker 2 two seconds after it starts.

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

Downloads run in the background, so the prompt stays usable while they run.

---

### 5. Shutting Down

Type `quit` in a client or tracker terminal. Closing a client logs its user out, so its files stop being offered to other peers.

---

##  Known Limitations

* **Tracker state lives in memory.** A tracker that restarts comes back empty, and the two trackers only connect once at startup; there is no state transfer when one rejoins.
* **Pieces are downloaded one at a time**, a new connection per piece, rotating through the seeders. There is no parallel download and no rarest-first piece selection, and a client starts seeding only once its download is complete.
* **Passwords are stored and sent in plain text**, and a peer serves any piece it has to anyone who asks, without checking group membership.
* **File names and paths cannot contain spaces**, because commands are space-separated.
* A command retried after failover may already have been applied by the tracker that died (for example, `create_group` then reports that the group exists).
