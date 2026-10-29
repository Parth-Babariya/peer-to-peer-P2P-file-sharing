#include "client.h"
#include "utils.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <random>
#include <algorithm>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <csignal>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <set>

using namespace std;

Client::Client(const string& tracker_info_file, long long upload_limit_kbps, bool partial_sharing)
    : partial_sharing(partial_sharing), upload_limit_bytes(upload_limit_kbps * 1024)
{
    int fd = open(tracker_info_file.c_str(), O_RDONLY);
    if (fd < 0) 
    {
        perror("Failed to open tracker_info.txt");
        exit(0);
    }

    char buffer[256];
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes_read <= 0) 
    {
        log_msg("Failed to read from tracker_info.txt or file is empty.");
        exit(EXIT_FAILURE);
    }
    buffer[bytes_read] = '\0'; // Null-terminate the buffer

    stringstream ss(buffer);
    string line;
    while(getline(ss, line)) 
    {
        if(!line.empty()) 
        {
            tracker_addresses.push_back(line);
        }
    }

    if (tracker_addresses.size() < 2) 
    {
        cerr << "FATAL: tracker_info.txt must contain at least two tracker addresses." << endl;
        exit(EXIT_FAILURE);
    }
}

void Client::run() 
{
    thread seeder_thread(&Client::start_seeder_service, this, open_seeder_socket());
    seeder_thread.detach();

    if (!connect_to_available_tracker()) 
    {
        return;
    }

    process_user_input();

    lock_guard<mutex> lock(tracker_mutex);
    if (tracker_socket != -1)
    {
        close(tracker_socket);
    }
}

bool Client::try_connect_to(const string& addr) 
{
    size_t delim_pos = addr.find(':');
    if (delim_pos == string::npos) 
        return false;
    string ip = addr.substr(0, delim_pos);
    int port = stoi(addr.substr(delim_pos + 1));

    tracker_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tracker_socket < 0) 
        return false;

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr);

    if (connect(tracker_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) 
    {
        close(tracker_socket);
        tracker_socket = -1;
        return false;
    }
    
    log_msg("successfully connected to tracker at " + addr);
    return true;
}

bool Client::connect_to_available_tracker() {
    if (try_connect_to(tracker_addresses[current_tracker_idx])) {
        return true;
    }

    log_msg("Could not connect to primary tracker. Failing over...");
    current_tracker_idx = (current_tracker_idx + 1) % tracker_addresses.size();
    
    if (try_connect_to(tracker_addresses[current_tracker_idx])) {
        return true;
    }

    cerr << "FATAL: Both trackers appear to be down." << endl;
    return false;
}

bool Client::request_reply(const string& command, string& response) {
    return tracker_socket >= 0 && send_msg(tracker_socket, command) && recv_msg(tracker_socket, response);
}

static bool is_catching_up(const string& reply) {
    return reply == CATCHING_UP_REPLY;
}

// How long to keep trying while trackers answer that they are catching up.
static const chrono::seconds CATCH_UP_WAIT(10);

// Connects to a tracker (the current one if it answers, else the other) and logs
// back in if we were logged in. Returns "" on success, else the reply to show.
// Caller holds tracker_mutex.
string Client::reconnect() {
    if (tracker_socket >= 0) {
        close(tracker_socket);
        tracker_socket = -1;
    }
    if (!connect_to_available_tracker()) {
        return "ERROR: All trackers are down.";
    }
    if (is_logged_in) {
        log_msg("Re-authenticating session with new tracker...");
        string login_cmd = make_message({"login", user_id, password, to_string(seeder_port)});
        string login_response;
        if (!request_reply(login_cmd, login_response)) {
            return "ERROR: Failed to send command to the secondary tracker.";
        }
        if (is_catching_up(login_response)) {
            return login_response;
        }
        if (login_response.find("success") == string::npos) {
            log_msg("Warning: Re-login failed. You may need to login manually.");
            is_logged_in = false;
        } else {
            log_msg("Re-authentication successful.");
        }
    }
    return "";
}

// Sends one command and returns the tracker's reply. If the connection is broken,
// fails over to the other tracker, logs back in, and retries the command once.
// A tracker that is still catching up sends us to the other one, back and forth,
// for up to CATCH_UP_WAIT.
string Client::send_to_tracker(const string& command) {
    lock_guard<mutex> lock(tracker_mutex);
    auto deadline = chrono::steady_clock::now() + CATCH_UP_WAIT;
    bool lost_once = false;
    while (true) {
        string response;
        if (request_reply(command, response)) {
            if (!is_catching_up(response)) {
                return response;
            }
        } else if (lost_once) {
            return "ERROR: Failed to send command to the secondary tracker.";
        } else {
            lost_once = true;
            log_msg("Connection lost. Attempting to reconnect and retry...");
            response = reconnect();
            if (response.empty()) continue;
        }

        // Catching up, here or at re-login: switch trackers and try again.
        while (is_catching_up(response)) {
            if (chrono::steady_clock::now() >= deadline) {
                return response;
            }
            this_thread::sleep_for(chrono::milliseconds(200));
            current_tracker_idx = (current_tracker_idx + 1) % tracker_addresses.size();
            response = reconnect();
        }
        if (!response.empty()) {
            return response;
        }
    }
}


// A tracker reply as the user should see it: names in a list come percent-encoded,
// so decode them, and quote any that contain spaces.
static string show_reply(const string& reply) {
    string shown;
    auto words = parse(reply, " ");
    for (size_t i = 0; i < words.size(); ++i) {
        vector<string> decoded;
        string word = words[i];
        if (decode_fields(word, decoded) && decoded[0] != word) {
            word = decoded[0].find(' ') == string::npos ? decoded[0] : "\"" + decoded[0] + "\"";
        }
        shown += (i > 0 ? " " : "") + word;
    }
    return shown;
}

void Client::process_user_input() {
    string line;
    while (cout << "> " && getline(cin, line)) {
        vector<string> args;
        if (!split_words(line, args)) {
            cout << "Unmatched or empty quotes." << endl;
            continue;
        }
        if (args.empty()) continue;

        const string& command = args[0];
        if (command == "quit") break;

        if (command == "login") {
            handle_login(args);
        } else if (command == "upload_file") {
            handle_upload(args);
        } else if (command == "download_file") {
            handle_download(args);
        } else if (command == "show_downloads") {
            show_downloads();
        } else {
            string response = send_to_tracker(make_message(args));
            cout << show_reply(response) << endl;
            if (command == "logout" && response.find("success") != string::npos) {
                lock_guard<mutex> lock(tracker_mutex);
                is_logged_in = false;
                user_id = "";
                password = "";
            }
        }
    }
}

void Client::handle_login(const vector<string>& args) {
    if (args.size() != 3) {
        cout << "Usage: login <user_id> <password>" << endl;
        return;
    }
    string command = make_message({args[0], args[1], args[2], to_string(seeder_port)});
    string response = send_to_tracker(command);
    cout << response << endl;

    if (response.find("success") != string::npos) {
        lock_guard<mutex> lock(tracker_mutex);
        is_logged_in = true;
        user_id = args[1];
        password = args[2];
    }
}

// Binds the seeder socket on a random free port. Runs before the tracker
// connection so seeder_port is set before login sends it.
int Client::open_seeder_socket() {
    int seeder_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (seeder_socket < 0) {
        perror("Seeder socket creation failed");
        exit(EXIT_FAILURE);
    }
    sockaddr_in seeder_addr{};
    seeder_addr.sin_family = AF_INET;
    seeder_addr.sin_addr.s_addr = INADDR_ANY;

    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> distrib(10000, 65000);

    while(true) {
        seeder_port = distrib(gen);
        seeder_addr.sin_port = htons(seeder_port);
        if (bind(seeder_socket, (struct sockaddr*)&seeder_addr, sizeof(seeder_addr)) == 0) {
            break;
        }
    }

    listen(seeder_socket, 10);
    log_msg("Seeder listening on port " + to_string(seeder_port));
    return seeder_socket;
}

void Client::start_seeder_service(int seeder_socket) {
    while (true) {
        int peer_sock = accept(seeder_socket, nullptr, nullptr);
        if (peer_sock < 0) continue;
        // Don't let a peer that connects and goes quiet hold a thread forever.
        timeval timeout{10, 0};
        setsockopt(peer_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        thread peer_handler(&Client::handle_peer_connection, this, peer_sock);
        peer_handler.detach();
    }
    close(seeder_socket);
}

// Serves two requests from peers, one per connection:
//   have <group> <file> <key>          -> one char per piece, '1' if we can serve it ("" if not sharing)
//   get_piece <group> <file> <key> <i> -> the piece's bytes ("" if we can't serve it)
// The key is the file's share key: the tracker gives it only to members of the
// file's group, so a peer that doesn't have it gets nothing. A file still
// downloading is served piece by piece, but only pieces already written and
// hash-checked.
void Client::handle_peer_connection(int peer_socket) {
    string request;
    if (!recv_msg(peer_socket, request)) {
        close(peer_socket);
        return;
    }
    vector<string> args;
    decode_fields(request, args); // a malformed request leaves args empty and gets ""

    string file_path;
    pair<string, string> file_id;
    if (args.size() >= 4) {
        file_id = {args[1], args[2]};
        lock_guard<mutex> lock(shared_files_mutex);
        auto it = shared_files.find(file_id);
        if (it != shared_files.end() && same_secret(args[3], it->second.key)) file_path = it->second.path;
    }

    // Pieces we hold, or empty if the file is complete (then we hold all of them).
    bool partial = false;
    vector<bool> held;
    if (!file_path.empty()) {
        lock_guard<mutex> lock(downloads_mutex);
        auto it = ongoing_downloads.find(file_id);
        if (it != ongoing_downloads.end() && it->second.status != "Completed") {
            partial = true;
            held = it->second.pieces_downloaded;
        }
    }

    if (args.size() == 4 && args[0] == "have") {
        string bits;
        if (partial) {
            for (bool b : held) bits += b ? '1' : '0';
        } else if (!file_path.empty()) {
            struct stat st;
            if (stat(file_path.c_str(), &st) == 0) {
                bits.assign((st.st_size + PIECE_SIZE - 1) / PIECE_SIZE, '1');
            }
        }
        send_msg(peer_socket, bits);
        close(peer_socket);
        return;
    }

    string piece;
    // Peers are untrusted: the index must be a plain non-negative number.
    bool valid = args.size() == 5 && args[0] == "get_piece" && !args[4].empty() && args[4].size() <= 9 &&
                 all_of(args[4].begin(), args[4].end(), ::isdigit);
    if (valid && !file_path.empty()) {
        long piece_index = stol(args[4]);
        bool have_it = !partial || (piece_index < (long)held.size() && held[piece_index]);
        int fd = have_it ? open(file_path.c_str(), O_RDONLY) : -1;
        if (fd != -1) {
            piece.resize(PIECE_SIZE);
            ssize_t bytes_read = pread(fd, &piece[0], PIECE_SIZE, (off_t)piece_index * PIECE_SIZE);
            piece.resize(bytes_read > 0 ? bytes_read : 0);
            close(fd);
        }
    }
    send_paced(peer_socket, piece);
    close(peer_socket);
}

// send_msg, but holding uploads to upload_limit_bytes per second across all peers.
void Client::send_paced(int sock, const string& data) {
    if (upload_limit_bytes <= 0) {
        send_msg(sock, data);
        return;
    }
    uint32_t len = htonl(static_cast<uint32_t>(data.size()));
    if (!send_all(sock, reinterpret_cast<const char*>(&len), sizeof(len))) return;
    const size_t chunk = 64 * 1024;
    for (size_t off = 0; off < data.size(); off += chunk) {
        size_t n = min(chunk, data.size() - off);
        pace(n);
        if (!send_all(sock, data.data() + off, n)) return;
    }
}

// Reserves the next slot of the shared upload budget and sleeps until it starts.
void Client::pace(size_t bytes) {
    chrono::steady_clock::time_point start;
    {
        lock_guard<mutex> lock(pace_mutex);
        auto now = chrono::steady_clock::now();
        start = max(now, pace_next);
        pace_next = start + chrono::nanoseconds((long long)(bytes * 1e9 / upload_limit_bytes));
    }
    this_thread::sleep_until(start);
}

void Client::handle_upload(const vector<string>& args) {
    if (args.size() != 3) {
        cout << "Usage: upload_file <group_id> <file_path>" << endl;
        return;
    }
    if (!is_logged_in) {
        cout << "You must be logged in to upload files." << endl;
        return;
    }

    const string& group_id = args[1];
    const string& file_path = args[2];
    string filename = file_path.substr(file_path.find_last_of("/\\") + 1);

    int fd = open(file_path.c_str(), O_RDONLY);
    if (fd < 0) {
        cout << "ERROR: Cannot open file " << file_path << endl;
        return;
    }
    
    struct stat file_stat;
    if (fstat(fd, &file_stat) < 0) {
        cout << "ERROR: Cannot get file stats for " << file_path << endl;
        close(fd);
        return;
    }
    long long file_size = file_stat.st_size;

    vector<string> piece_hashes;
    vector<char> piece_buffer(PIECE_SIZE);
    Sha1 file_hash;

    ssize_t bytes_read;
    while((bytes_read = read(fd, piece_buffer.data(), PIECE_SIZE)) > 0) {
        piece_hashes.push_back(sha(piece_buffer.data(), bytes_read));
        file_hash.update(piece_buffer.data(), bytes_read);
    }
    close(fd);
    string full_file_hash = file_hash.hex();
    string share_key = random_hex(16);

    vector<string> command = {"upload_file", group_id, filename, to_string(file_size), full_file_hash, share_key};
    command.insert(command.end(), piece_hashes.begin(), piece_hashes.end());

    string response = send_to_tracker(make_message(command));
    cout << response << endl;

    if (response.find("success") != string::npos) {
        lock_guard<mutex> lock(shared_files_mutex);
        shared_files[{group_id, filename}] = {file_path, share_key};
    }
}

void Client::handle_download(const vector<string>& args) {
    if (args.size() != 4) {
        cout << "Usage: download_file <group_id> <file_name> <destination_path>" << endl;
        return;
    }
    if (!is_logged_in) {
        cout << "You must be logged in." << endl;
        return;
    }

    string response = send_to_tracker(make_message({"download_file", args[1], args[2]}));

    vector<string> metadata;
    if (decode_fields(response, metadata) && metadata[0] == "success") {
        log_msg("Starting download for " + args[2]);
        thread downloader(&Client::download_manager, this, args[1], args[2], args[3], metadata);
        downloader.detach();
    } else {
        cout << response << endl;
    }
}

// Parallel download tuning.
static const int DOWNLOAD_WORKERS = 8;          // pieces in flight at once per download
static const int MAX_REQUESTS_PER_SEEDER = 2;   // spreads one download over many seeders
static const int MAX_FRUITLESS_REFRESHES = 3;   // give up when this many refreshes find nothing
static const chrono::milliseconds REFRESH_INTERVAL(300);   // when out of work, ask again this often
static const chrono::milliseconds PERIODIC_REFRESH(1000);  // and at least this often regardless

// Connects to a peer's "ip:port"; returns the socket, or -1.
static int connect_to_peer(const string& addr) {
    size_t delim_pos = addr.find(':');
    if (delim_pos == string::npos) return -1;
    sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    try {
        peer_addr.sin_port = htons(stoi(addr.substr(delim_pos + 1)));
    } catch (const exception&) {
        return -1;
    }
    if (inet_pton(AF_INET, addr.substr(0, delim_pos).c_str(), &peer_addr.sin_addr) != 1) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    // A seeder that accepts but never answers must not stall the download forever.
    timeval timeout{10, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(sock, (struct sockaddr*)&peer_addr, sizeof(peer_addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

// One request and its reply, on a fresh connection to a peer.
static bool ask_peer(const string& addr, const string& request, string& reply) {
    int sock = connect_to_peer(addr);
    if (sock < 0) return false;
    bool ok = send_msg(sock, request) && recv_msg(sock, reply);
    close(sock);
    return ok;
}

// What the worker threads of one download share. Guarded by m.
struct Swarm {
    mutex m;
    condition_variable cv;
    vector<bool> done;
    vector<bool> in_flight;
    int remaining = 0;
    int in_flight_count = 0;
    bool failed = false;
    bool refreshing = false;
    int fruitless_refreshes = 0;
    chrono::steady_clock::time_point last_refresh{};
    map<string, string> bitfields; // live seeder -> which pieces it has ("0110...")
    map<string, int> busy;         // seeder -> requests in flight to it
    set<string> dead;              // seeders dropped for the rest of this download
};

void Client::download_manager(const string& group_id, const string& filename, const string& dest_path, const vector<string>& metadata) {
    auto set_status = [&](const string& status) {
        lock_guard<mutex> lock(downloads_mutex);
        ongoing_downloads[{group_id, filename}].status = status;
    };

    // metadata: success <file_size> <file_hash> <share_key> <piece_hash>... <seeder>...
    DownloadState state;
    state.group_id = group_id;
    state.filename = filename;
    state.destination_path = dest_path;
    state.status = "Downloading";
    try {
        state.file_size = stoll(metadata.at(1));
    } catch (const exception&) {
        state.file_size = -1;
    }
    state.total_pieces = state.file_size < 0 ? 0 : (state.file_size + PIECE_SIZE - 1) / PIECE_SIZE;
    const size_t seeder_start_index = 4 + state.total_pieces;
    if (state.file_size < 0 || metadata.size() < seeder_start_index) {
        log_msg("Malformed file info from tracker. Download failed for " + filename);
        lock_guard<mutex> lock(downloads_mutex);
        state.status = "Failed";
        ongoing_downloads[{group_id, filename}] = state;
        return;
    }
    const string& file_hash = metadata[2];
    const string& share_key = metadata[3];
    const int n = state.total_pieces;
    state.pieces_downloaded.resize(n, false);
    for (int i = 0; i < n; ++i) {
        state.piece_hashes[i] = metadata[4 + i];
    }

    {
        lock_guard<mutex> lock(downloads_mutex);
        ongoing_downloads[{group_id, filename}] = state;
    }

    // Full size up front, so pieces can be written in any order.
    int fd = open(dest_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0 || ftruncate(fd, state.file_size) < 0) {
        log_msg("Failed to create destination file: " + dest_path + ". Download failed for " + filename);
        if (fd >= 0) close(fd);
        set_status("Failed");
        return;
    }

    // Advertise ourselves right away: other downloaders can take the pieces we
    // already have (the seeder side only serves verified ones).
    if (partial_sharing) {
        {
            lock_guard<mutex> share_lock(shared_files_mutex);
            shared_files[{group_id, filename}] = {dest_path, share_key};
        }
        send_to_tracker(make_message({"i_am_seeder", group_id, filename}));
    }

    Swarm swarm;
    swarm.done.assign(n, false);
    swarm.in_flight.assign(n, false);
    swarm.remaining = n;
    for (size_t i = seeder_start_index; i < metadata.size(); ++i) {
        swarm.bitfields[metadata[i]] = ""; // not asked yet
    }
    mt19937 rng(random_device{}());

    // Learns who has which pieces: asks the tracker for the current seeders (new
    // partial seeders appear as others start the same download), then asks each one
    // for its bitfield. Drops swarm.m while it waits on the network.
    auto refresh = [&](unique_lock<mutex>& lk) {
        swarm.refreshing = true;
        set<string> candidates;
        for (const auto& entry : swarm.bitfields) candidates.insert(entry.first);
        lk.unlock();

        vector<string> reply;
        if (decode_fields(send_to_tracker(make_message({"download_file", group_id, filename})), reply) &&
            reply[0] == "success") {
            for (size_t i = seeder_start_index; i < reply.size(); ++i) candidates.insert(reply[i]);
        }
        map<string, string> found;
        set<string> unreachable;
        for (const auto& seeder : candidates) {
            string bits;
            if (ask_peer(seeder, make_message({"have", group_id, filename, share_key}), bits)) {
                found[seeder] = bits.size() == (size_t)n ? bits : string(n, '0');
            } else {
                unreachable.insert(seeder);
            }
        }

        lk.lock();
        for (const auto& seeder : unreachable) {
            swarm.dead.insert(seeder);
            swarm.bitfields.erase(seeder);
        }
        for (const auto& [seeder, bits] : found) {
            if (!swarm.dead.count(seeder)) swarm.bitfields[seeder] = bits;
        }
        swarm.refreshing = false;
        swarm.last_refresh = chrono::steady_clock::now();
        swarm.cv.notify_all();
    };

    // Rarest first: of the pieces still needed and not being fetched, take one held
    // by the fewest live seeders (ties broken randomly, so concurrent downloaders
    // spread out), from its least busy holder that has a free request slot.
    auto pick = [&](int& piece, string& seeder) {
        vector<pair<int, int>> candidates; // (holders, piece)
        for (int p = 0; p < n; ++p) {
            if (swarm.done[p] || swarm.in_flight[p]) continue;
            int holders = 0;
            for (const auto& entry : swarm.bitfields) {
                if (entry.second.size() == (size_t)n && entry.second[p] == '1') holders++;
            }
            if (holders > 0) candidates.push_back({holders, p});
        }
        shuffle(candidates.begin(), candidates.end(), rng);
        stable_sort(candidates.begin(), candidates.end(),
                    [](const pair<int, int>& a, const pair<int, int>& b) { return a.first < b.first; });
        for (const auto& candidate : candidates) {
            int p = candidate.second;
            string best;
            int best_busy = INT_MAX;
            for (const auto& [addr, bits] : swarm.bitfields) {
                if (bits.size() != (size_t)n || bits[p] != '1') continue;
                int b = swarm.busy[addr];
                if (b < MAX_REQUESTS_PER_SEEDER && b < best_busy) {
                    best = addr;
                    best_busy = b;
                }
            }
            if (!best.empty()) {
                piece = p;
                seeder = best;
                return true;
            }
        }
        return false;
    };

    enum class Fetch { Ok, NotThere, Bad };
    auto fetch = [&](const string& seeder, int p, string& piece) {
        int sock = connect_to_peer(seeder);
        if (sock < 0) return Fetch::Bad;
        log_msg("Downloading piece " + to_string(p) + " from seeder " + seeder);
        bool ok = send_msg(sock, make_message({"get_piece", group_id, filename, share_key, to_string(p)})) && recv_msg(sock, piece);
        close(sock);
        if (!ok) return Fetch::Bad;
        if (piece.empty()) return Fetch::NotThere; // its bitfield was out of date
        long long expected_size = min<long long>(PIECE_SIZE, state.file_size - (long long)p * PIECE_SIZE);
        if ((long long)piece.size() != expected_size || sha(piece.data(), piece.size()) != state.piece_hashes[p]) {
            return Fetch::Bad;
        }
        return Fetch::Ok;
    };

    auto worker = [&]() {
        unique_lock<mutex> lk(swarm.m);
        while (swarm.remaining > 0 && !swarm.failed) {
            // Refresh even while there is work: a full seeder always has something to
            // offer, and without this we'd never notice that other downloaders now hold
            // pieces too. One worker refreshes; the others keep downloading.
            if (!swarm.refreshing && chrono::steady_clock::now() - swarm.last_refresh >= PERIODIC_REFRESH) {
                refresh(lk);
                continue;
            }
            int p;
            string seeder;
            if (!pick(p, seeder)) {
                if (swarm.refreshing || chrono::steady_clock::now() - swarm.last_refresh < REFRESH_INTERVAL) {
                    swarm.cv.wait_for(lk, chrono::milliseconds(100));
                    continue;
                }
                refresh(lk);
                // Nothing to fetch, nothing in flight, and asking again didn't help:
                // after a few rounds no reachable seeder has what we still need.
                if (!pick(p, seeder) && swarm.in_flight_count == 0 &&
                    ++swarm.fruitless_refreshes >= MAX_FRUITLESS_REFRESHES) {
                    swarm.failed = true;
                    swarm.cv.notify_all();
                }
                continue;
            }

            swarm.in_flight[p] = true;
            swarm.in_flight_count++;
            swarm.busy[seeder]++;
            lk.unlock();

            string piece;
            Fetch result = fetch(seeder, p, piece);
            bool written = result == Fetch::Ok &&
                           pwrite(fd, piece.data(), piece.size(), (off_t)p * PIECE_SIZE) == (ssize_t)piece.size();

            lk.lock();
            swarm.in_flight[p] = false;
            swarm.in_flight_count--;
            swarm.busy[seeder]--;
            if (written) {
                swarm.done[p] = true;
                swarm.remaining--;
                swarm.fruitless_refreshes = 0;
                lock_guard<mutex> lock(downloads_mutex);
                ongoing_downloads[{group_id, filename}].pieces_downloaded[p] = true;
            } else if (result == Fetch::Ok) {
                log_msg("Write to " + dest_path + " failed.");
                swarm.failed = true;
            } else if (result == Fetch::NotThere) {
                auto it = swarm.bitfields.find(seeder);
                if (it != swarm.bitfields.end()) it->second[p] = '0';
            } else if (!swarm.dead.count(seeder)) {
                log_msg("Seeder " + seeder + " failed or sent a bad copy of piece " + to_string(p) + "; dropping it.");
                swarm.dead.insert(seeder);
                swarm.bitfields.erase(seeder);
            }
            swarm.cv.notify_all();
        }
    };

    vector<thread> workers;
    for (int i = 0; i < min(DOWNLOAD_WORKERS, n); ++i) {
        workers.emplace_back(worker);
    }
    for (auto& t : workers) {
        t.join();
    }
    close(fd);

    auto give_up = [&](const string& why) {
        log_msg("Download failed for " + filename + ": " + why);
        set_status("Failed");
        if (partial_sharing) {
            {
                lock_guard<mutex> share_lock(shared_files_mutex);
                shared_files.erase({group_id, filename});
            }
            send_to_tracker(make_message({"stop_share", group_id, filename}));
        }
    };
    if (swarm.remaining > 0) {
        give_up("no reachable seeder has the missing pieces.");
        return;
    }
    // Every piece matched its hash; check the whole file as well.
    if (sha_file(dest_path) != file_hash) {
        give_up("whole-file hash mismatch.");
        return;
    }

    log_msg("Download completed for " + filename);
    set_status("Completed");
    if (!partial_sharing) {
        {
            lock_guard<mutex> share_lock(shared_files_mutex);
            shared_files[{group_id, filename}] = {dest_path, share_key};
        }
        // Never hold downloads_mutex here: this waits on the network.
        send_to_tracker(make_message({"i_am_seeder", group_id, filename}));
    }
}

void Client::show_downloads() {
    lock_guard<mutex> lock(downloads_mutex);
    if (ongoing_downloads.empty()) {
        cout << "No active or completed downloads." << endl;
        return;
    }
    for (const auto& pair : ongoing_downloads) {
        const auto& state = pair.second;
        if (state.status == "Completed") {
            cout << "[C] [" << state.group_id << "] " << state.filename << endl;
        } else if (state.status == "Failed") {
            cout << "[F] [" << state.group_id << "] " << state.filename << endl;
        } else {
            int done = count(state.pieces_downloaded.begin(), state.pieces_downloaded.end(), true);
            cout << "[D] [" << state.group_id << "] " << state.filename
                 << " (" << done << "/" << state.total_pieces << " pieces)" << endl;
        }
    }
}

int main(int argc, char* argv[]) {
    const string usage = "Usage: ./client <tracker_info.txt> [--upload-limit <KB/s>] [--no-partial-sharing]";
    if (argc < 2) {
        cerr << usage << endl;
        return 1;
    }
    long long upload_limit_kbps = 0;
    bool partial_sharing = true;
    for (int i = 2; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "--upload-limit" && i + 1 < argc) {
            try {
                upload_limit_kbps = stoll(argv[++i]);
            } catch (const exception&) {
                upload_limit_kbps = -1;
            }
            if (upload_limit_kbps < 0) {
                cerr << usage << endl;
                return 1;
            }
        } else if (arg == "--no-partial-sharing") {
            partial_sharing = false;
        } else {
            cerr << usage << endl;
            return 1;
        }
    }

    // A peer hanging up mid-send must give an error, not kill the process.
    signal(SIGPIPE, SIG_IGN);

    string tracker_info_file = argv[1];

    Client client(tracker_info_file, upload_limit_kbps, partial_sharing);
    client.run();

    // Download and seeder threads are detached and still use `client`; returning
    // would destroy it under them. End the process without running destructors.
    cout.flush();
    _exit(0);
}