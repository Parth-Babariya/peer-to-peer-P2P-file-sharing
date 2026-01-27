
## Distributed File Sharing System (P2P)

This project is a **peer-to-peer (P2P) file sharing system** implemented in **C++**.
It allows users to form groups, share files, and download files directly from other peers. The system uses **two redundant tracker servers** to ensure reliability, and clients automatically failover if the primary tracker becomes unavailable.

This implementation is designed with the constraint of using only **standard system calls and libraries** (no high-level file or networking libraries).

---

##  Dependencies

To compile and run this project, ensure you have:

* A **C++ compiler** with C++11 support (e.g., `g++`).
* `make` build utility.
* **OpenSSL development libraries** (for SHA1 hashing).

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

---

##  Directory Structure

```
.
├── client/
│   ├── client.cpp
│   ├── client.h
│   ├── utils.cpp
│   ├── utils.h
│   └── Makefile
├── tracker/
│   ├── tracker.cpp
│   ├── tracker.h
│   ├── utils.cpp
│   ├── utils.h
│   └── Makefile
├── tracker_info.txt    # You must create this file
└── README.md
```

---

##  Compilation Instructions

You need to compile **tracker** and **client** separately.

### Compile the Tracker

```bash
cd tracker/
make clean && make
```

### Compile the Client

```bash
cd ../client/
make clean && make
```

This creates two executables:

* `tracker/tracker`
* `client/client`

---

##  Execution Instructions

You will need at least **three separate terminals**.

### 1. Create the Tracker Configuration File

Before starting, create a file named `tracker_info.txt` in the **root directory**:

Example `tracker_info.txt`:

```
127.0.0.1:8080
127.0.0.1:8081
```

This file tells the clients and trackers where to find the tracker servers.

---

### 2. Start the Tracker Servers

Open **two terminals**:

**Terminal 1 (Start Tracker 1):**

```bash
./tracker/tracker tracker_info.txt 1
```

**Terminal 2 (Start Tracker 2):**

```bash
./tracker/tracker tracker_info.txt 2
```

Both trackers will now run and synchronize.

---

### 3. Start a Client

Open another terminal for each client:

```bash
./client/client tracker_info.txt
```

The client first attempts to connect to the **first tracker**. If it fails, it automatically connects to the **second tracker**.

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
  list_requests <group_id>
  accept_request <group_id> <user_id>
  ```

* **File Sharing**

  ```
  upload_file <group_id> <file_path>
  list_files <group_id>
  download_file <group_id> <file_name> <destination_path>
  show_downloads
  ```

---

### 5. Shutting Down

To shut down a tracker, simply type:

```
quit
```

in its terminal.
