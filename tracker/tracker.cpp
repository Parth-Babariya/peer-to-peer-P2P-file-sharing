#include "tracker.h"
#include "utils.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <csignal>
#include <algorithm>
#include <fstream>
#include <cerrno>

using namespace std;

// While a state-changing command runs, its reply is captured here instead of
// sent, so it can be recorded and synced first (see process_command).
static thread_local string* captured_reply = nullptr;

void send_response(int sock, const string& msg) {
    if (captured_reply) {
        *captured_reply = msg;
        return;
    }
    send_msg(sock, msg);
}

Tracker::Tracker(const string& info_file, int tracker_num, const string& state_file)
    : tracker_id(tracker_num), state_file(state_file) {
    int fd = open(info_file.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("Failed to open tracker_info.txt");
        exit(EXIT_FAILURE);
    }

    char buffer[256];
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes_read <= 0) {
        log_msg("Failed to read from tracker_info.txt or file is empty.");
        exit(EXIT_FAILURE);
    }
    buffer[bytes_read] = '\0'; // Null-terminate the buffer to treat it as a string

    stringstream ss(buffer);
    string line1, line2;
    getline(ss, line1);
    getline(ss, line2);

    string my_line = (tracker_id == 1) ? line1 : line2;
    string other_line = (tracker_id == 1) ? line2 : line1;
    
    size_t delim_pos = my_line.find(':');
    ip_addr = my_line.substr(0, delim_pos);
    port = stoi(my_line.substr(delim_pos + 1));
    
    delim_pos = other_line.find(':');
    other_tracker_addr = other_line.substr(0, delim_pos);
    other_tracker_port = stoi(other_line.substr(delim_pos + 1));

    log_msg("Tracker " + to_string(tracker_id) + " starting at " + ip_addr + ":" + to_string(port));
    log_msg("Other tracker at " + other_tracker_addr + ":" + to_string(other_tracker_port));
}

void Tracker::start() {
    load_state();

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip_addr.c_str());
    server_addr.sin_port = htons(port);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 10) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    log_msg("Tracker listening for clients on port " + to_string(port));

    thread client_thread(&Tracker::listen_for_clients, this);
    thread tracker_thread(&Tracker::listen_for_tracker, this);
    
    // Tracker 1 dials and tracker 2 listens, both for as long as they run, so the
    // pair reconnects whichever one restarts.
    if (tracker_id == 1) {
        thread connect_thread(&Tracker::connect_to_other_tracker, this);
        connect_thread.detach();
    }
    thread(&Tracker::wait_to_serve_alone, this).detach();
    
    string command;
    cout << "Tracker console running. Type 'quit' to shut down." << endl;
    while (cin >> command) {
        if (command == "quit") {
            // Skip static destructors: client and sync threads are still running.
            cout.flush();
            _exit(0);
        } else if (command == "disconnect") {
            // Cut the sync link and keep it down, as a network split would.
            sync_enabled = false;
            lock_guard<mutex> lock(other_tracker_socket_mutex);
            if (other_tracker_socket != -1) shutdown(other_tracker_socket, SHUT_RDWR);
            log_msg("Sync link disabled.");
        } else if (command == "reconnect") {
            sync_enabled = true;
            log_msg("Sync link enabled.");
        }
    }
    // stdin closed (e.g. running in the background): keep serving.

    close(server_socket);
    client_thread.join();
    tracker_thread.join();
}


// --- Client Connection Handling ---
void Tracker::listen_for_clients() {
    while (true) {
        sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);
        int client_socket = accept(server_socket, (struct sockaddr*)&client_address, &client_len);
        if (client_socket < 0) {
            log_msg("Accept failed or server shut down.");
            break;
        }
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_address.sin_addr, client_ip, INET_ADDRSTRLEN);
        
        string client_addr_str(client_ip);
        log_msg("New client connection from " + client_addr_str);

        thread t(&Tracker::handle_client, this, client_socket, client_addr_str);
        t.detach();
    }
}

void Tracker::handle_client(int client_socket, const string& client_addr) {
    string command_str;
    while (recv_msg(client_socket, command_str)) {
        vector<string> args;
        if (!caught_up) {
            send_response(client_socket, CATCHING_UP_REPLY);
        } else if (decode_fields(command_str, args)) {
            process_command(client_socket, client_addr, args);
        } else {
            send_response(client_socket, "error :  Bad arguments");
        }
    }

    string user_id = get_user_id_from_socket(client_socket);
    if (!user_id.empty()) {
        end_session(client_socket, user_id);
    }
    log_msg("Client " + client_addr + " disconnected.");
    close(client_socket);
}

// Commands whose repeat would not be harmless (a second create_group fails).
static const set<string> STATE_CHANGING = {
    "create_user", "create_group", "join_group", "leave_group", "accept_request",
    "upload_file", "stop_share", "i_am_seeder",
};
static const size_t MAX_REMEMBERED_RESULTS = 10000;

// args may start with "#<request_id>". The client sends a fresh id per command
// and the same id when it retries that command after a failover.
void Tracker::process_command(int sock, const string& client_addr, vector<string> args) {
    string request_id;
    if (args[0].size() > 1 && args[0][0] == '#') {
        request_id = args[0].substr(1);
        args.erase(args.begin());
        if (args.empty()) {
            send_response(sock, "error :  Bad arguments");
            return;
        }
    }
    bool once_only = !request_id.empty() && STATE_CHANGING.count(args[0]);
    string reply;
    if (once_only && find_result(request_id, reply)) {
        log_msg("Replaying the reply to retried request " + request_id + ".");
        send_response(sock, reply);
        return;
    }

    if (once_only) captured_reply = &reply;
    // A malformed number (stoll) or similar must cost the client an error, not crash the tracker.
    try {
        dispatch_command(sock, client_addr, args);
    } catch (const exception& e) {
        send_response(sock, "error :  Bad arguments");
        log_msg(string("Rejected malformed command: ") + e.what());
    }
    if (!once_only) return;
    captured_reply = nullptr;

    // The change itself was synced by the handler; the result follows, and only
    // then does the client hear back. A tracker that dies before replying has
    // passed on both, unless it dies in the moment between the two messages.
    remember_result(request_id, reply);
    send_sync_message(make_message({"synced_RESULT", request_id, reply}));
    send_response(sock, reply);
}

bool Tracker::find_result(const string& request_id, string& reply) {
    lock_guard<mutex> lock(results_mutex);
    auto it = results.find(request_id);
    if (it == results.end()) return false;
    reply = it->second;
    return true;
}

void Tracker::remember_result(const string& request_id, const string& reply) {
    lock_guard<mutex> lock(results_mutex);
    if (results.count(request_id)) return;
    results[request_id] = reply;
    results_order.push_back(request_id);
    if (results_order.size() > MAX_REMEMBERED_RESULTS) {
        results.erase(results_order.front());
        results_order.pop_front();
    }
}

void Tracker::dispatch_command(int sock, const string& client_addr, const vector<string>& args) {
    const string& command = args[0];
    if (command == "create_user") create_user(sock, args);
    else if (command == "login") login(sock, client_addr, args);
    else if (command == "create_group") create_group(sock, args);
    else if (command == "join_group") join_group(sock, args);
    else if (command == "leave_group") leave_group(sock, args);
    else if (command == "list_requests") list_requests(sock, args);
    else if (command == "accept_request") accept_request(sock, args);
    else if (command == "list_groups") list_groups(sock);
    else if (command == "list_files") list_files(sock, args);
    else if (command == "upload_file") upload_file(sock, args);
    else if (command == "download_file") download_file(sock, args);
    else if (command == "logout") logout(sock, args);
    else if (command == "stop_share") stop_share(sock, args);
    else if (command == "i_am_seeder") i_am_seeder(sock, args);
    else {
        send_response(sock, "error : Invalid command");
    }
}

// --- Command Handler Implementations ---

void Tracker::create_user(int sock, const vector<string>& args) {
    if (args.size() != 3) {
        send_response(sock, "error : Usage: create_user <user_id> <password>");
        return;
    }
    const string& user_id = args[1];
    // Only a salted hash is kept, and only it is synced. Hashing is slow on
    // purpose, so it happens before taking the lock.
    string record = hash_password(args[2]);

    lock_guard<mutex> lock(users_mutex);
    if (users.contains(user_id)) {
        send_response(sock, "error : User already exists");
    } else {
        Stamp stamp = next_stamp();
        users.apply(user_id, true, record, stamp);
        send_response(sock, "success User created");
        log_msg("User " + user_id + " created.");
        send_sync_message(make_message({"synced_USER", user_id, record, stamp.str()}));
    }
}

void Tracker::login(int sock, const string& client_addr_str, const vector<string>& args) {
    if (args.size() != 4) {
        send_response(sock, "error :  Usage: login <user_id> <password> <port>");
        return;
    }
    const string& user_id = args[1];
    const string& password = args[2];
    const string& client_port = args[3];

    string record;
    {
        lock_guard<mutex> lock(users_mutex);
        record = users.value(user_id);
    }
    // Slow on purpose, so no lock is held. An unknown user is checked against a
    // dummy record, so the reply time doesn't reveal which user ids exist.
    static const string dummy_record = hash_password("");
    bool valid = check_password(password, record.empty() ? dummy_record : record) && !record.empty();
    if (!valid) {
        send_response(sock, "error :  Invalid credentials");
        return;
    }

    lock_guard<mutex> session_lock(logged_in_users_mutex);
    lock_guard<mutex> socket_lock(socket_to_user_mutex);

    if (logged_in_users.contains(user_id)) {
        log_msg("User " + user_id + " is re-establishing session from a new connection.");
        int old_sock = -1;
        for(auto const& [key_sock, val_user] : socket_to_user) {
            if (val_user == user_id) {
                old_sock = key_sock;
                break;
            }
        }
        if (old_sock != -1) {
            socket_to_user.erase(old_sock);
        }
    }

    string client_full_addr = client_addr_str + ":" + client_port;
    Stamp stamp = next_stamp();
    logged_in_users.apply(user_id, true, client_full_addr, stamp);
    socket_to_user[sock] = user_id;
    send_response(sock, "success Login successful");
    log_msg("User " + user_id + " logged in from " + client_full_addr);
    send_sync_message(make_message({"synced_SESSION", user_id, "1", client_full_addr, stamp.str()}));
}

void Tracker::logout(int sock, const vector<string>& args) {
    if (args.size() != 1) {
        send_response(sock, "error :  Usage: logout");
        return;
    }
    // Only ever the session on this connection: a user id from the client is not trusted.
    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) {
        send_response(sock, "error :  Not logged in");
        return;
    }
    end_session(sock, user_id);
    send_response(sock, "success Logout successful");
}

// Logs the user out and stops advertising them as a seeder, here and on the other tracker.
void Tracker::end_session(int sock, const string& user_id) {
    string user_addr = get_address_from_user_id(user_id);

    {
        lock_guard<mutex> session_lock(logged_in_users_mutex);
        lock_guard<mutex> socket_lock(socket_to_user_mutex);
        Stamp stamp = next_stamp();
        logged_in_users.apply(user_id, false, "", stamp);
        socket_to_user.erase(sock);
        send_sync_message(make_message({"synced_SESSION", user_id, "0", "-", stamp.str()}));
    }

    if (!user_addr.empty()) {
        lock_guard<mutex> group_lock(groups_mutex);
        Stamp stamp = next_stamp();
        for (auto& group_pair : groups) {
            for (auto& file_pair : group_pair.second.files) {
                if (file_pair.second.seeders.contains(user_addr)) {
                    set_seeder(group_pair.second, file_pair.second, user_addr, false, stamp);
                }
            }
        }
    }

    log_msg("User " + user_id + " logged out.");
}

void Tracker::create_group(int sock, const vector<string>& args) {
    if (args.size() != 2) {
        send_response(sock, "error :  Usage: create_group <group_id>");
        return;
    }
    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) {
        send_response(sock, "error :  Not logged in");
        return;
    }
    const string& group_id = args[1];
    
    lock_guard<mutex> lock(groups_mutex);
    if(groups.count(group_id)) {
        send_response(sock, "error :  Group already exists.");
        return;
    }

    Stamp stamp = next_stamp();
    Group& group = groups[group_id];
    group.group_id = group_id;
    group.owner_id = user_id;
    group.owner_stamp = stamp;
    send_sync_message(make_message({"synced_GROUP", group_id, user_id, stamp.str()}));
    set_member(group, user_id, true, stamp);

    send_response(sock, "success Group created.");
}

void Tracker::join_group(int sock, const vector<string>& args) {
    if (args.size() != 2) {
        send_response(sock, "error :  Usage: join_group <group_id>");
        return;
    }
    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) {
        send_response(sock, "error :  Not logged in");
        return;
    }
    const string& group_id = args[1];

    lock_guard<mutex> lock(groups_mutex);
    if(!groups.count(group_id)) {
        send_response(sock, "error :  Group does not exist.");
        return;
    }
    Group& group = groups.at(group_id);
    if(group.members.contains(user_id)) {
        send_response(sock, "error :  You are already a member.");
        return;
    }

    set_pending(group, user_id, true, next_stamp());
    send_response(sock, "success Join request sent.");
}

void Tracker::leave_group(int sock, const vector<string>& args) {
    if (args.size() != 2) {
        send_response(sock, "error :  Usage: leave_group <group_id>");
        return;
    }
    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) {
        send_response(sock, "error :  Not logged in");
        return;
    }
    const string& group_id = args[1];
    
    lock_guard<mutex> lock(groups_mutex);
    if(!groups.count(group_id)) {
        send_response(sock, "error :  Group does not exist.");
        return;
    }
    Group& group = groups.at(group_id);
    if(!group.members.contains(user_id)) {
        send_response(sock, "error :  You are not a member of this group.");
        return;
    }

    set_member(group, user_id, false, next_stamp());
    send_response(sock, "success You have left the group.");
}


void Tracker::list_requests(int sock, const vector<string>& args) {
    if (args.size() != 2) {
        send_response(sock, "error :  Usage: list_requests <group_id>");
        return;
    }
    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) {
        send_response(sock, "error :  Not logged in");
        return;
    }
    const string& group_id = args[1];

    lock_guard<mutex> lock(groups_mutex);
    if(!groups.count(group_id)) {
        send_response(sock, "error :  Group does not exist.");
        return;
    }
    Group& group = groups.at(group_id);
    if(group.owner_id != user_id) {
        send_response(sock, "error :  You are not the owner of this group.");
        return;
    }
    
    string response = "success ";
    if(group.pending_requests.empty()) {
        response += "No pending requests.";
    } else {
        for(const auto& req_user : group.pending_requests.keys()) {
            response += encode_field(req_user) + " ";
        }
    }
    send_response(sock, response);
}

void Tracker::accept_request(int sock, const vector<string>& args) {
    if (args.size() != 3) {
        send_response(sock, "error :  Usage: accept_request <group_id> <user_id>");
        return;
    }
    string owner_id = get_user_id_from_socket(sock);
    if (owner_id.empty()) {
        send_response(sock, "error :  Not logged in");
        return;
    }
    const string& group_id = args[1];
    const string& user_to_accept = args[2];

    lock_guard<mutex> lock(groups_mutex);
    if (!groups.count(group_id)) {
        send_response(sock, "error :  Group does not exist.");
        return;
    }
    Group& group = groups.at(group_id);
    if (group.owner_id != owner_id) {
        send_response(sock, "error :  You are not the owner of this group.");
        return;
    }
    if (!group.pending_requests.contains(user_to_accept)) {
        send_response(sock, "error :  This user has not requested to join.");
        return;
    }

    Stamp stamp = next_stamp();
    set_pending(group, user_to_accept, false, stamp);
    set_member(group, user_to_accept, true, stamp);
    send_response(sock, "success User added to group.");
}

void Tracker::list_groups(int sock) {
    lock_guard<mutex> lock(groups_mutex);
    string response = "success ";
    if (groups.empty()) {
        response += "No groups available.";
    } else {
        for (const auto& pair : groups) {
            response += encode_field(pair.first) + " ";
        }
    }
    send_response(sock, response);
}

void Tracker::list_files(int sock, const vector<string>& args) {
    if (args.size() != 2) {
        send_response(sock, "error :  Usage: list_files <group_id>");
        return;
    }
    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) {
        send_response(sock, "error :  Not logged in.");
        return;
    }
    const string& group_id = args[1];

    lock_guard<mutex> lock(groups_mutex);
    if (!groups.count(group_id)) {
        send_response(sock, "error :  Group does not exist.");
        return;
    }
    if (!groups.at(group_id).members.contains(user_id)) {
        send_response(sock, "error :  Not a member of this group.");
        return;
    }
    const auto& files = groups.at(group_id).files;
    string response = "success ";
    if (files.empty()) {
        response += "No files in this group.";
    } else {
        for (const auto& pair : files) {
            response += encode_field(pair.first) + " ";
        }
    }
    send_response(sock, response);
}

// A share key is 128 random bits as lowercase hex.
static bool is_share_key(const string& s) {
    return s.size() == 32 && all_of(s.begin(), s.end(), [](char c) { return isdigit(c) || (c >= 'a' && c <= 'f'); });
}

// upload_file <group_id> <file_name> <size> <file_sha1> <share_key> <piece_sha1>...
void Tracker::upload_file(int sock, const vector<string>& args) {
    if (args.size() < 6) {
        send_response(sock, "error :  Invalid upload command format.");
        return;
    }

    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) {
        send_response(sock, "error :  You must be logged in to upload.");
        return;
    }

    const string& group_id = args[1];
    const string& filename = args[2];

    // Look this up before taking groups_mutex: see the lock order in tracker.h.
    string client_addr = get_address_from_user_id(user_id);
    if(client_addr.empty()) {
        send_response(sock, "error :  Could not find your address info.");
        return;
    }

    lock_guard<mutex> lock(groups_mutex);
    if (groups.find(group_id) == groups.end()) {
        send_response(sock, "error :  Group does not exist.");
        return;
    }

    Group& group = groups.at(group_id);
    if (!group.members.contains(user_id)) {
        send_response(sock, "error :  You are not a member of this group.");
        return;
    }

    long long file_size = stoll(args[3]);
    if (!is_share_key(args[5])) {
        send_response(sock, "error :  Malformed share key.");
        return;
    }
    long long expected_pieces = (file_size + PIECE_SIZE - 1) / PIECE_SIZE;
    if (file_size < 0 || (long long)args.size() - 6 != expected_pieces) {
        send_response(sock, "error :  Piece hash count does not match the file size.");
        return;
    }

    Stamp stamp = next_stamp();
    FileInfo& file = group.files[filename];
    file.filename = filename;
    file.file_size = file_size;
    file.file_hash = args[4];
    file.share_key = args[5];
    file.piece_hashes.clear();
    for (size_t i = 6; i < args.size(); ++i) {
        file.piece_hashes[i-6] = args[i];
    }
    file.stamp = stamp;
    vector<string> sync_fields = args;
    sync_fields[0] = "synced_FILE";
    sync_fields.push_back(stamp.str());
    send_sync_message(make_message(sync_fields));

    // A re-upload replaces the file, so whoever seeded the old one stops counting.
    for (const auto& addr : file.seeders.keys()) {
        if (addr != client_addr) set_seeder(group, file, addr, false, stamp);
    }
    set_seeder(group, file, client_addr, true, stamp);

    send_response(sock, "success File uploaded successfully.");
    log_msg("File " + filename + " uploaded to group " + group_id + " by " + user_id);
}

void Tracker::download_file(int sock, const vector<string>& args) {
    if (args.size() != 3) {
        send_response(sock, "error :  Usage: download_file <group_id> <file_name>");
        return;
    }
    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) {
        send_response(sock, "error :  Not logged in.");
        return;
    }

    const string& group_id = args[1];
    const string& filename = args[2];
    // A client that is already seeding part of this file doesn't need itself listed.
    string requester_addr = get_address_from_user_id(user_id);

    lock_guard<mutex> lock(groups_mutex);
    if (!groups.count(group_id)) {
        send_response(sock, "error :  Group does not exist.");
        return;
    }
    Group& group = groups.at(group_id);
    if (!group.members.contains(user_id)) {
        send_response(sock, "error :  Not a member of this group.");
        return;
    }
    if (!group.files.count(filename)) {
        send_response(sock, "error :  File not found in this group.");
        return;
    }

    FileInfo& file = group.files.at(filename);
    vector<string> seeders;
    for (const auto& seeder : file.seeders.keys()) {
        if (seeder != requester_addr) seeders.push_back(seeder);
    }
    if (seeders.empty()) {
        send_response(sock, "error :  No seeders available for this file.");
        return;
    }

    string hashes_log = "Sending piece hashes for " + filename + " to " + user_id + ":";
    for (size_t i = 0; i < file.piece_hashes.size(); ++i) {
        hashes_log += " " + file.piece_hashes.at(i);
    }
    log_msg(hashes_log);

    stringstream response;
    // Only members get this far, so only members learn the share key.
    response << "success " << file.file_size << " " << file.file_hash << " " << file.share_key;
    for (size_t i = 0; i < file.piece_hashes.size(); ++i) {
        response << " " << file.piece_hashes.at(i);
    }
    for (const auto& seeder : seeders) {
        response << " " << seeder;
    }
    send_response(sock, response.str());
}

void Tracker::stop_share(int sock, const vector<string>& args) {
    if (args.size() != 3) {
        send_response(sock, "error :  Usage: stop_share <group_id> <file_name>");
        return;
    }
    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) {
        send_response(sock, "error :  Not logged in.");
        return;
    }
    
    const string& group_id = args[1];
    const string& filename = args[2];
    string user_addr = get_address_from_user_id(user_id);
    
    lock_guard<mutex> lock(groups_mutex);
    if (groups.count(group_id) && groups.at(group_id).files.count(filename)) {
        Group& group = groups.at(group_id);
        FileInfo& file = group.files.at(filename);
        if (file.seeders.contains(user_addr)) set_seeder(group, file, user_addr, false, next_stamp());
        send_response(sock, "success No longer sharing file.");
    } else {
        send_response(sock, "error File or group not found.");
    }
}

void Tracker::i_am_seeder(int sock, const vector<string>& args) {
    // Every command gets exactly one reply; the client waits for it.
    if (args.size() != 3) {
        send_response(sock, "error :  Usage: i_am_seeder <group_id> <file_name>");
        return;
    }
    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) {
        send_response(sock, "error :  Not logged in.");
        return;
    }

    const string& group_id = args[1];
    const string& filename = args[2];
    string seeder_addr = get_address_from_user_id(user_id);
    if (seeder_addr.empty()) {
        send_response(sock, "error :  Could not find your address info.");
        return;
    }

    lock_guard<mutex> lock(groups_mutex);
    if(groups.count(group_id) && groups.at(group_id).files.count(filename)) {
        Group& group = groups.at(group_id);
        set_seeder(group, group.files.at(filename), seeder_addr, true, next_stamp());
        log_msg("User " + user_id + " is now a seeder for " + filename);
        send_response(sock, "success Now seeding " + filename);
    } else {
        send_response(sock, "error File or group not found.");
    }
}

// --- Helper Methods ---
string Tracker::get_user_id_from_socket(int sock) {
    lock_guard<mutex> lock(socket_to_user_mutex);
    if (socket_to_user.count(sock)) {
        return socket_to_user.at(sock);
    }
    return "";
}

string Tracker::get_address_from_user_id(const string& user_id) {
    lock_guard<mutex> lock(logged_in_users_mutex);
    return logged_in_users.value(user_id);
}

Stamp Tracker::next_stamp() {
    Stamp stamp;
    stamp.counter = ++clock;
    stamp.tracker = tracker_id;
    return stamp;
}

void Tracker::observe(const Stamp& stamp) {
    unsigned long long seen = clock.load();
    while (seen < stamp.counter && !clock.compare_exchange_weak(seen, stamp.counter)) {
    }
}

// Each applies one change here and forwards it; the caller holds groups_mutex.
void Tracker::set_member(Group& group, const string& user_id, bool present, const Stamp& stamp) {
    group.members.apply(user_id, present, "", stamp);
    send_sync_message(make_message({"synced_MEMBER", group.group_id, user_id, present ? "1" : "0", stamp.str()}));
}

void Tracker::set_pending(Group& group, const string& user_id, bool present, const Stamp& stamp) {
    group.pending_requests.apply(user_id, present, "", stamp);
    send_sync_message(make_message({"synced_PENDING", group.group_id, user_id, present ? "1" : "0", stamp.str()}));
}

void Tracker::set_seeder(Group& group, FileInfo& file, const string& addr, bool present, const Stamp& stamp) {
    file.seeders.apply(addr, present, "", stamp);
    send_sync_message(make_message({"synced_SEEDER", group.group_id, file.filename, addr, present ? "1" : "0",
                                    stamp.str()}));
}

// --- Tracker Synchronization Logic ---

void Tracker::listen_for_tracker() {
    int listener_socket = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listener_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in sync_addr{};
    sync_addr.sin_family = AF_INET;
    sync_addr.sin_addr.s_addr = INADDR_ANY;
    sync_addr.sin_port = htons(port + 100);

    if (bind(listener_socket, (struct sockaddr*)&sync_addr, sizeof(sync_addr)) < 0) {
        log_msg("sync bind failed on port " + to_string(port+100));
        close(listener_socket);
        return;
    }
    listen(listener_socket, 1);

    log_msg("Listening for other tracker on port " + to_string(port + 100));

    while (true) {
        int sync_sock = accept(listener_socket, nullptr, nullptr);
        if (sync_sock < 0) continue;
        if (!sync_enabled) {
            close(sync_sock);
            continue;
        }
        log_msg("Other tracker connected for synchronization.");
        run_sync_session(sync_sock);
    }
}

void Tracker::connect_to_other_tracker() {
    bool reported = false;
    while (true) {
        if (!sync_enabled) {
            this_thread::sleep_for(chrono::milliseconds(200));
            continue;
        }
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(other_tracker_port + 100);
        server_addr.sin_addr.s_addr = inet_addr(other_tracker_addr.c_str());

        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(sock);
            if (!reported) {
                log_msg("Other tracker unreachable. Running standalone and retrying every second.");
                reported = true;
            }
            this_thread::sleep_for(chrono::seconds(1));
            continue;
        }

        log_msg("Connected to other tracker.");
        reported = false;
        run_sync_session(sock);
    }
}

// One sync connection, from connect to disconnect. Both sides send their whole
// state first, then every later change, so a tracker that restarted empty
// catches up. Returns when the connection is lost.
void Tracker::run_sync_session(int sync_socket) {
    sync_session_active = true;
    // Start reading before sending our snapshot: if both snapshots are larger than
    // the socket buffers and neither side reads, both sends would block forever.
    thread reader(&Tracker::handle_sync_connection, this, sync_socket);
    send_snapshot_and_attach(sync_socket);
    reader.join();
    sync_session_active = false;
}

// How long a tracker that can't reach the other one waits before serving alone.
static const chrono::seconds STANDALONE_GRACE(3);

// Opens the doors to clients if the other tracker can't be reached at all. While
// a sync session is open its snapshot is on the way, so keep waiting for it.
void Tracker::wait_to_serve_alone() {
    this_thread::sleep_for(STANDALONE_GRACE);
    while (!caught_up) {
        if (!sync_session_active) {
            caught_up = true;
            log_msg("Other tracker unreachable; serving clients from our own state.");
            return;
        }
        this_thread::sleep_for(chrono::milliseconds(100));
    }
}

// Sends the full state as ordinary sync messages, then makes this socket the
// target of live sync messages. The snapshot is taken with every state lock held,
// and other_tracker_socket_mutex is taken before they are released, so each change
// is either already in the snapshot or sent after it, never lost in between.
// Tombstones are sent too, and applying the same message twice is harmless.
void Tracker::send_snapshot_and_attach(int sync_socket) {
    unique_lock<mutex> users_lock(users_mutex);
    unique_lock<mutex> session_lock(logged_in_users_mutex);
    unique_lock<mutex> socket_lock(socket_to_user_mutex);
    unique_lock<mutex> groups_lock(groups_mutex);

    vector<string> snapshot = state_messages();

    lock_guard<mutex> lock(other_tracker_socket_mutex);
    groups_lock.unlock();
    socket_lock.unlock();
    session_lock.unlock();
    users_lock.unlock();

    // The end marker lets the other side know (and log) when it has caught up.
    snapshot.push_back("synced_SNAPSHOT_END " + to_string(snapshot.size()));
    for (const auto& msg : snapshot) {
        if (!send_msg(sync_socket, msg)) {
            shutdown(sync_socket, SHUT_RDWR); // the reader thread notices and closes it
            return;
        }
    }
    log_msg("Sent state snapshot (" + to_string(snapshot.size() - 1) + " messages) to other tracker.");
    other_tracker_socket = sync_socket;
}

// The whole state, tombstones included, as sync messages. Caller holds every
// state lock.
vector<string> Tracker::state_messages() {
    vector<string> snapshot;
    auto flag = [](const LwwEntry& e) { return e.present ? "1" : "0"; };
    for (const auto& [user_id, e] : users.entries()) {
        snapshot.push_back(make_message({"synced_USER", user_id, e.value, e.stamp.str()}));
    }
    for (const auto& [user_id, e] : logged_in_users.entries()) {
        snapshot.push_back(make_message({"synced_SESSION", user_id, flag(e), e.present ? e.value : "-", e.stamp.str()}));
    }
    for (const auto& [group_id, group] : groups) {
        snapshot.push_back(make_message({"synced_GROUP", group_id, group.owner_id, group.owner_stamp.str()}));
        for (const auto& [user_id, e] : group.members.entries()) {
            snapshot.push_back(make_message({"synced_MEMBER", group_id, user_id, flag(e), e.stamp.str()}));
        }
        for (const auto& [user_id, e] : group.pending_requests.entries()) {
            snapshot.push_back(make_message({"synced_PENDING", group_id, user_id, flag(e), e.stamp.str()}));
        }
        for (const auto& [filename, file] : group.files) {
            vector<string> msg = {"synced_FILE", group_id, filename, to_string(file.file_size), file.file_hash,
                                  file.share_key};
            for (size_t i = 0; i < file.piece_hashes.size(); ++i) {
                msg.push_back(file.piece_hashes.at(i));
            }
            msg.push_back(file.stamp.str());
            snapshot.push_back(make_message(msg));
            for (const auto& [addr, e] : file.seeders.entries()) {
                snapshot.push_back(make_message({"synced_SEEDER", group_id, filename, addr, flag(e), e.stamp.str()}));
            }
        }
    }
    return snapshot;
}

void Tracker::handle_sync_connection(int sync_socket) {
    string sync_command_str;
    while (recv_msg(sync_socket, sync_command_str)) {
        vector<string> args;
        if (decode_fields(sync_command_str, args)) {
            process_sync_command(args);
        } else {
            log_msg("Ignoring malformed sync message.");
        }
    }
    log_msg("Connection with other tracker lost.");
    {
        lock_guard<mutex> lock(other_tracker_socket_mutex);
        if (other_tracker_socket == sync_socket) {
            other_tracker_socket = -1;
        }
    }
    // Only this thread closes the socket, so its fd number can't be reused by
    // another connection while a read on it is still in progress.
    close(sync_socket);
}

static bool well_formed(const vector<string>& args);

// Writes all of `data`, looping over partial writes.
static bool write_all(int fd, const string& data) {
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

// Replays the state file, then rewrites it as one snapshot (so it doesn't keep
// growing across restarts) and keeps it open for appending.
void Tracker::load_state() {
    size_t applied = 0;
    {
        ifstream in(state_file, ios::binary);
        string line;
        while (getline(in, line)) {
            if (in.eof()) break; // no newline: a write cut short by a crash
            vector<string> args;
            if (!decode_fields(line, args) || !well_formed(args)) continue;
            try {
                apply_sync_command(args);
                applied++;
            } catch (const exception&) {
            }
        }
    }

    vector<string> messages;
    {
        lock_guard<mutex> users_lock(users_mutex);
        lock_guard<mutex> session_lock(logged_in_users_mutex);
        lock_guard<mutex> socket_lock(socket_to_user_mutex);
        lock_guard<mutex> groups_lock(groups_mutex);
        messages = state_messages();
    }
    string data;
    for (const auto& msg : messages) data += msg + "\n";

    // Write the compacted copy beside the file, then swap it in with rename(),
    // which is atomic: a crash leaves either the old file or the new one.
    string tmp = state_file + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool ok = fd >= 0 && write_all(fd, data) && fsync(fd) == 0;
    if (fd >= 0) close(fd);
    ok = ok && rename(tmp.c_str(), state_file.c_str()) == 0;
    if (ok) {
        size_t slash = state_file.find_last_of('/');
        string dir = slash == string::npos ? "." : state_file.substr(0, slash + 1);
        int dir_fd = open(dir.c_str(), O_RDONLY);
        if (dir_fd >= 0) {
            fsync(dir_fd); // makes the rename itself durable
            close(dir_fd);
        }
        state_fd = open(state_file.c_str(), O_WRONLY | O_APPEND);
    }
    if (state_fd < 0) {
        log_msg("Warning: cannot write " + state_file + "; state will not survive a restart.");
        return;
    }
    log_msg("Loaded " + to_string(applied) + " changes from " + state_file + " (" +
            to_string(messages.size()) + " after compaction).");
}

// Appends one state change and waits until it is on disk.
void Tracker::persist(const string& message) {
    lock_guard<mutex> lock(state_file_mutex);
    if (state_fd < 0) return;
    if (!write_all(state_fd, message + "\n") || fdatasync(state_fd) != 0) {
        log_msg("Warning: failed to write " + state_file + ".");
    }
}

void Tracker::send_sync_message(const string& message) {
    // Replies to retried requests are only a cache; everything else is state.
    if (message.rfind("synced_RESULT ", 0) != 0) persist(message);
    lock_guard<mutex> lock(other_tracker_socket_mutex);
    if (other_tracker_socket != -1) {
        if (!send_msg(other_tracker_socket, message)) {
            log_msg("Failed to send sync message. Other tracker may be down.");
            shutdown(other_tracker_socket, SHUT_RDWR); // the reader thread closes it
            other_tracker_socket = -1;
        } else {
            // Only the type: a user record holds a password hash.
            log_msg("Sent sync message: " + message.substr(0, message.find(' ')));
        }
    }
}

// A known sync message with at least the fields it needs.
static bool well_formed(const vector<string>& args) {
static const map<string, size_t> min_args = {
        {"synced_USER", 4}, {"synced_SESSION", 5}, {"synced_GROUP", 4}, {"synced_MEMBER", 5},
        {"synced_PENDING", 5}, {"synced_FILE", 7}, {"synced_SEEDER", 6},
        {"synced_RESULT", 3}, {"synced_SNAPSHOT_END", 2},
    };
    auto rule = min_args.find(args[0]);
    return rule != min_args.end() && args.size() >= rule->second;
}

void Tracker::process_sync_command(const vector<string>& args) {
    const string& command = args[0];
    log_msg("Received sync command: " + args[0]);

    if (!well_formed(args)) {
        log_msg("Ignoring malformed sync message: " + command);
        return;
    }
    try {
        apply_sync_command(args);
    } catch (const exception& e) {
        log_msg(string("Ignoring malformed sync message: ") + e.what());
        return;
    }
    if (command != "synced_RESULT" && command != "synced_SNAPSHOT_END") {
        persist(make_message(args));
    }
}

// Every state change ends with its stamp and is applied last-writer-wins, so
// changes may arrive twice or in any order. A change to a group or file that we
// don't have is dropped: messages on the link (and snapshots) put the group and
// file first.
void Tracker::apply_sync_command(const vector<string>& args) {
    const string& command = args[0];

    if (command == "synced_RESULT") {
        remember_result(args[1], args[2]);
        return;
    }
    if (command == "synced_SNAPSHOT_END") {
        caught_up = true;
        log_msg("Caught up with other tracker's state (" + args[1] + " messages).");
        return;
    }

    Stamp stamp;
    if (!Stamp::parse(args.back(), stamp)) {
        throw invalid_argument("bad stamp " + args.back());
    }
    observe(stamp);

    if (command == "synced_USER") {
        lock_guard<mutex> lock(users_mutex);
        users.apply(args[1], true, args[2], stamp);
    } else if (command == "synced_SESSION") {
        lock_guard<mutex> lock(logged_in_users_mutex);
        logged_in_users.apply(args[1], args[2] == "1", args[3], stamp);
    } else if (command == "synced_GROUP") {
        lock_guard<mutex> lock(groups_mutex);
        Group& group = groups[args[1]];
        group.group_id = args[1];
        if (group.owner_id.empty() || group.owner_stamp < stamp) {
            group.owner_id = args[2];
            group.owner_stamp = stamp;
        }
    } else if (command == "synced_MEMBER" || command == "synced_PENDING") {
        lock_guard<mutex> lock(groups_mutex);
        auto it = groups.find(args[1]);
        if (it == groups.end()) return;
        LwwMap& users_in = command == "synced_MEMBER" ? it->second.members : it->second.pending_requests;
        users_in.apply(args[2], args[3] == "1", "", stamp);
    } else if (command == "synced_FILE") {
        lock_guard<mutex> lock(groups_mutex);
        auto it = groups.find(args[1]);
        if (it == groups.end()) return;
        FileInfo& file = it->second.files[args[2]];
        file.filename = args[2];
        if (file.stamp < stamp) {
            file.file_size = stoll(args[3]);
            file.file_hash = args[4];
            file.share_key = args[5];
            file.piece_hashes.clear();
            for (size_t i = 6; i + 1 < args.size(); ++i) file.piece_hashes[i - 6] = args[i];
            file.stamp = stamp;
        }
    } else if (command == "synced_SEEDER") {
        lock_guard<mutex> lock(groups_mutex);
        auto it = groups.find(args[1]);
        if (it == groups.end() || !it->second.files.count(args[2])) return;
        it->second.files.at(args[2]).seeders.apply(args[3], args[4] == "1", "", stamp);
    }
}

// --- Main Function ---
int main(int argc, char* argv[]) {
    const string usage = "Usage: ./tracker <tracker_info_file> <tracker_no> [--state-file <path>]";
    if (argc != 3 && !(argc == 5 && string(argv[3]) == "--state-file")) {
        cerr << usage << endl;
        return 1;
    }
    string info_file = argv[1];
    string tracker_arg = argv[2];
    if (tracker_arg != "1" && tracker_arg != "2") {
        cerr << "tracker_no must be 1 or 2" << endl;
        return 1;
    }
    int tracker_num = stoi(tracker_arg);

    // A client hanging up mid-reply must give an error, not kill the tracker.
    signal(SIGPIPE, SIG_IGN);

    string state_file = argc == 5 ? argv[4] : "tracker" + tracker_arg + ".state";
    Tracker tracker(info_file, tracker_num, state_file);
    tracker.start();

    return 0;
}