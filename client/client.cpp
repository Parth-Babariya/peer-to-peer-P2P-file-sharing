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

using namespace std;

Client::Client(const string& tracker_info_file) 
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

// Sends one command and returns the tracker's reply. If the connection is broken,
// fails over to the other tracker, logs back in, and retries the command once.
string Client::send_to_tracker(const string& command) {
    lock_guard<mutex> lock(tracker_mutex);
    string response;
    if (request_reply(command, response)) {
        return response;
    }

    log_msg("Connection lost. Attempting to reconnect and retry...");
    if (tracker_socket >= 0) {
        close(tracker_socket);
        tracker_socket = -1;
    }
    if (!connect_to_available_tracker()) {
        return "ERROR: All trackers are down.";
    }

    if (is_logged_in) {
        log_msg("Re-authenticating session with new tracker...");
        string login_cmd = "login " + user_id + " " + password + " " + to_string(seeder_port);
        string login_response;
        if (!request_reply(login_cmd, login_response) || login_response.find("success") == string::npos) {
            log_msg("Warning: Re-login failed. You may need to login manually.");
            is_logged_in = false;
        } else {
            log_msg("Re-authentication successful.");
        }
    }

    if (request_reply(command, response)) {
        return response;
    }
    return "ERROR: Failed to send command to the secondary tracker.";
}


void Client::process_user_input() {
    string line;
    while (cout << "> " && getline(cin, line)) {
        auto args = parse(line, " ");
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
            string response = send_to_tracker(line);
            cout << response << endl;
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
    string command = args[0] + " " + args[1] + " " + args[2] + " " + to_string(seeder_port);
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

void Client::handle_peer_connection(int peer_socket) {
    string request;
    if (!recv_msg(peer_socket, request)) {
        close(peer_socket);
        return;
    }

    // The reply is always one message: the piece's bytes, or empty if we can't serve it.
    string piece;
    auto args = parse(request, " ");
    // Peers are untrusted: the index must be a plain non-negative number.
    bool valid = args.size() == 3 && args[0] == "get_piece" && !args[2].empty() && args[2].size() <= 9 &&
                 all_of(args[2].begin(), args[2].end(), ::isdigit);
    if (valid) {
        const string& filename = args[1];
        long piece_index = stol(args[2]);

        string file_path;
        {
            lock_guard<mutex> lock(shared_files_mutex);
            if (shared_files.count(filename)) {
                file_path = shared_files.at(filename);
            }
        }

        if (!file_path.empty()) {
            int fd = open(file_path.c_str(), O_RDONLY);
            if (fd != -1) {
                piece.resize(PIECE_SIZE);
                ssize_t bytes_read = pread(fd, &piece[0], PIECE_SIZE, (off_t)piece_index * PIECE_SIZE);
                piece.resize(bytes_read > 0 ? bytes_read : 0);
                close(fd);
            }
        }
    }
    send_msg(peer_socket, piece);
    close(peer_socket);
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

    stringstream command_stream;
    command_stream << "upload_file " << group_id << " " << filename << " " << file_size << " " << full_file_hash;
    for(const auto& p_hash : piece_hashes) {
        command_stream << " " << p_hash;
    }

    string response = send_to_tracker(command_stream.str());
    cout << response << endl;

    if (response.find("success") != string::npos) {
        lock_guard<mutex> lock(shared_files_mutex);
        shared_files[filename] = file_path;
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

    string command = "download_file " + args[1] + " " + args[2];
    string response = send_to_tracker(command);
    
    auto metadata = parse(response, " ");
    if (metadata[0] == "success") {
        log_msg("Starting download for " + args[2]);
        thread downloader(&Client::download_manager, this, args[1], args[2], args[3], metadata);
        downloader.detach();
    } else {
        cout << response << endl;
    }
}

// Connects to a seeder's "ip:port"; returns the socket, or -1.
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

void Client::download_manager(const string& group_id, const string& filename, const string& dest_path, const vector<string>& metadata) {
    auto set_status = [&](const string& status) {
        lock_guard<mutex> lock(downloads_mutex);
        ongoing_downloads[filename].status = status;
    };

    // metadata: success <file_size> <file_hash> <piece_hash>... <seeder>...
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
    size_t seeder_start_index = 3 + state.total_pieces;
    if (state.file_size < 0 || metadata.size() < seeder_start_index) {
        log_msg("Malformed file info from tracker. Download failed for " + filename);
        lock_guard<mutex> lock(downloads_mutex);
        state.status = "Failed";
        ongoing_downloads[filename] = state;
        return;
    }
    const string& file_hash = metadata[2];
    state.pieces_downloaded.resize(state.total_pieces, false);
    for (int i = 0; i < state.total_pieces; ++i) {
        state.piece_hashes[i] = metadata[3 + i];
    }
    vector<string> seeders(metadata.begin() + seeder_start_index, metadata.end());

    {
        lock_guard<mutex> lock(downloads_mutex);
        ongoing_downloads[filename] = state;
    }

    int fd = open(dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        log_msg("Failed to create destination file: " + dest_path + ". Download failed for " + filename);
        set_status("Failed");
        return;
    }

    // A seeder that can't be reached, hangs up, or sends a piece with the wrong
    // hash is dropped for the rest of this download. When none are left, the
    // download fails instead of retrying forever.
    size_t seeder_idx = 0;
    for (int i = 0; i < state.total_pieces; ++i) {
        long long expected_size = min<long long>(PIECE_SIZE, state.file_size - (long long)i * PIECE_SIZE);
        bool piece_ok = false;
        while (!piece_ok) {
            if (seeders.empty()) {
                log_msg("No more seeders. Download failed for " + filename);
                close(fd);
                set_status("Failed");
                return;
            }
            seeder_idx %= seeders.size();
            string seeder_addr = seeders[seeder_idx];

            int peer_sock = connect_to_peer(seeder_addr);
            string piece;
            string problem;
            if (peer_sock < 0) {
                problem = "could not connect";
            } else {
                log_msg("Downloading piece " + to_string(i) + " from seeder " + seeder_addr);
                string request = "get_piece " + filename + " " + to_string(i);
                if (!send_msg(peer_sock, request) || !recv_msg(peer_sock, piece)) {
                    problem = "disconnected while sending piece " + to_string(i);
                } else if ((long long)piece.size() != expected_size ||
                           sha(piece.data(), piece.size()) != state.piece_hashes[i]) {
                    problem = "sent a bad copy of piece " + to_string(i);
                }
                close(peer_sock);
            }

            if (!problem.empty()) {
                log_msg("Seeder " + seeder_addr + " " + problem + "; dropping it.");
                seeders.erase(seeders.begin() + seeder_idx);
                continue;
            }

            if (pwrite(fd, piece.data(), piece.size(), (off_t)i * PIECE_SIZE) != (ssize_t)piece.size()) {
                log_msg("Write to " + dest_path + " failed. Download failed for " + filename);
                close(fd);
                set_status("Failed");
                return;
            }
            piece_ok = true;
            seeder_idx++; // spread consecutive pieces across seeders
            lock_guard<mutex> lock(downloads_mutex);
            ongoing_downloads[filename].pieces_downloaded[i] = true;
        }
    }
    close(fd);

    // Every piece matched its hash; check the whole file as well.
    if (sha_file(dest_path) != file_hash) {
        log_msg("Whole-file hash mismatch. Download failed for " + filename);
        set_status("Failed");
        return;
    }

    log_msg("Download completed for " + filename);
    set_status("Completed");
    {
        lock_guard<mutex> share_lock(shared_files_mutex);
        shared_files[filename] = dest_path;
    }

    // Never hold downloads_mutex here: this waits on the network.
    string command = "i_am_seeder " + group_id + " " + filename;
    send_to_tracker(command);
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
    if (argc != 2) {
        cerr << "Usage: ./client <tracker_info.txt>" << endl;
        return 1;
    }

    // A peer hanging up mid-send must give an error, not kill the process.
    signal(SIGPIPE, SIG_IGN);

    string tracker_info_file = argv[1];

    Client client(tracker_info_file);
    client.run();

    return 0;
}