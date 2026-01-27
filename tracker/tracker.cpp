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

using namespace std;

#define size_piece 512*1024

void send_response(int sock, const string& msg) {
    send(sock, msg.c_str(), msg.length(), 0);
}

Tracker::Tracker(const string& info_file, int tracker_num) : tracker_id(tracker_num) {
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
    
    if (tracker_id == 1) {
        this_thread::sleep_for(chrono::seconds(2));
        thread connect_thread(&Tracker::connect_to_other_tracker, this);
        connect_thread.detach();
    }
    
    string command;
    cout << "Tracker console running. Type 'quit' to shut down." << endl;
    while (true) {
        cin >> command;
        if (command == "quit") {
            exit(0);
        }
    }

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
    char buffer[size_piece] = {0};
    while (read(client_socket, buffer, size_piece) > 0) {
        string command_str(buffer);
        auto args = parse(command_str, " ");
        if (!args.empty()) {
            process_command(client_socket, client_addr, args);
        }
        memset(buffer, 0, size_piece);
    }

    string user_id = get_user_id_from_socket(client_socket);
    if (!user_id.empty()) {
        vector<string> logout_args = {"logout", user_id};
        logout(client_socket, logout_args);
    }
    log_msg("Client " + client_addr + " disconnected.");
    close(client_socket);
}

void Tracker::process_command(int sock, const string& client_addr, const vector<string>& args) {
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
    const string& password = args[2];

    lock_guard<mutex> lock(users_mutex);
    if (users.count(user_id)) {
        send_response(sock, "error : User already exists");
    } else {
        users[user_id] = password;
        send_response(sock, "success User created");
        log_msg("User " + user_id + " created.");
        send_sync_message("synced_CREATE_USER " + user_id + " " + password);
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

    lock_guard<mutex> user_lock(users_mutex);
    lock_guard<mutex> session_lock(logged_in_users_mutex);
    lock_guard<mutex> socket_lock(socket_to_user_mutex);

    if (users.find(user_id) == users.end() || users.at(user_id) != password) {
        send_response(sock, "error :  Invalid credentials");
        return;
    }

    if (logged_in_users.count(user_id)) {
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
    logged_in_users[user_id] = client_full_addr;
    socket_to_user[sock] = user_id;
    send_response(sock, "success Login successful");
    log_msg("User " + user_id + " logged in from " + client_full_addr);
    send_sync_message("synced_LOGIN " + user_id + " " + client_full_addr);
}

void Tracker::logout(int sock, const vector<string>& args) {
    string user_id;
    if (args.size() > 1) { 
        user_id = args[1];
    } else {
        user_id = get_user_id_from_socket(sock);
    }

    if (user_id.empty()) {
        send_response(sock, "error :  Not logged in");
        return;
    }
    
    string user_addr = get_address_from_user_id(user_id);

    {
        lock_guard<mutex> session_lock(logged_in_users_mutex);
        lock_guard<mutex> socket_lock(socket_to_user_mutex);
        logged_in_users.erase(user_id);
        socket_to_user.erase(sock);
    }
    
    {
        lock_guard<mutex> group_lock(groups_mutex);
        for (auto& group_pair : groups) {
            for (auto& file_pair : group_pair.second.files) {
                file_pair.second.seeders.erase(user_addr);
            }
        }
    }

    send_response(sock, "success Logout successful");
    log_msg("User " + user_id + " logged out.");
    send_sync_message("synced_LOGOUT " + user_id + " " + user_addr);
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

    Group new_group;
    new_group.group_id = group_id;
    new_group.owner_id = user_id;
    new_group.members.insert(user_id);
    groups[group_id] = new_group;

    send_response(sock, "success Group created.");
    send_sync_message("synced_CREATE_GROUP " + group_id + " " + user_id);
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
    if(group.members.count(user_id)) {
        send_response(sock, "error :  You are already a member.");
        return;
    }

    group.pending_requests.insert(user_id);
    send_response(sock, "success Join request sent.");
    send_sync_message("synced_JOIN_GROUP " + group_id + " " + user_id);
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
    if(!group.members.count(user_id)) {
        send_response(sock, "error :  You are not a member of this group.");
        return;
    }
    
    group.members.erase(user_id);
    send_response(sock, "success You have left the group.");
    send_sync_message("synced_LEAVE_GROUP " + group_id + " " + user_id);
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
        for(const auto& req_user : group.pending_requests) {
            response += req_user + " ";
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
    if (!group.pending_requests.count(user_to_accept)) {
        send_response(sock, "error :  This user has not requested to join.");
        return;
    }

    group.pending_requests.erase(user_to_accept);
    group.members.insert(user_to_accept);
    send_response(sock, "success User added to group.");
    send_sync_message("synced_ACCEPT_REQUEST " + group_id + " " + user_to_accept);
}

void Tracker::list_groups(int sock) {
    lock_guard<mutex> lock(groups_mutex);
    string response = "success ";
    if (groups.empty()) {
        response += "No groups available.";
    } else {
        for (const auto& pair : groups) {
            response += pair.first + " ";
        }
    }
    send_response(sock, response);
}

void Tracker::list_files(int sock, const vector<string>& args) {
    if (args.size() != 2) {
        send_response(sock, "error :  Usage: list_files <group_id>");
        return;
    }
    const string& group_id = args[1];
    
    lock_guard<mutex> lock(groups_mutex);
    if (!groups.count(group_id)) {
        send_response(sock, "error :  Group does not exist.");
        return;
    }
    const auto& files = groups.at(group_id).files;
    string response = "success ";
    if (files.empty()) {
        response += "No files in this group.";
    } else {
        for (const auto& pair : files) {
            response += pair.first + " ";
        }
    }
    send_response(sock, response);
}

void Tracker::upload_file(int sock, const vector<string>& args) {
    if (args.size() < 5) {
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
    
    lock_guard<mutex> lock(groups_mutex);
    if (groups.find(group_id) == groups.end()) {
        send_response(sock, "error :  Group does not exist.");
        return;
    }

    Group& group = groups.at(group_id);
    if (group.members.find(user_id) == group.members.end()) {
        send_response(sock, "error :  You are not a member of this group.");
        return;
    }
    
    FileInfo new_file;
    new_file.filename = filename;
    new_file.file_size = stoll(args[3]);
    new_file.file_hash = args[4];
    
    for (size_t i = 5; i < args.size(); ++i) {
        new_file.piece_hashes[i-5] = args[i];
    }

    string client_addr = get_address_from_user_id(user_id);
    if(client_addr.empty()) {
        send_response(sock, "error :  Could not find your address info.");
        return;
    }
    new_file.seeders.insert(client_addr);

    group.files[filename] = new_file;
    
    send_response(sock, "success File uploaded successfully.");
    log_msg("File " + filename + " uploaded to group " + group_id + " by " + user_id);

    stringstream sync_msg_stream;
    sync_msg_stream << "synced_UPLOAD " << group_id << " " << filename << " " << args[3] << " " << args[4];
    for(size_t i = 5; i < args.size(); ++i) {
        sync_msg_stream << " " << args[i];
    }
    sync_msg_stream << " " << client_addr;
    send_sync_message(sync_msg_stream.str());
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

    lock_guard<mutex> lock(groups_mutex);
    if (!groups.count(group_id)) {
        send_response(sock, "error :  Group does not exist.");
        return;
    }
    Group& group = groups.at(group_id);
    if (!group.members.count(user_id)) {
        send_response(sock, "error :  Not a member of this group.");
        return;
    }
    if (!group.files.count(filename)) {
        send_response(sock, "error :  File not found in this group.");
        return;
    }

    FileInfo& file = group.files.at(filename);
    if (file.seeders.empty()) {
        send_response(sock, "error :  No seeders available for this file.");
        return;
    }

    string hashes_log = "Sending piece hashes for " + filename + " to " + user_id + ":";
    for (size_t i = 0; i < file.piece_hashes.size(); ++i) {
        hashes_log += " " + file.piece_hashes.at(i);
    }
    log_msg(hashes_log);

    stringstream response;
    response << "success " << file.file_size << " " << file.file_hash;
    for (size_t i = 0; i < file.piece_hashes.size(); ++i) {
        response << " " << file.piece_hashes.at(i);
    }
    for (const auto& seeder : file.seeders) {
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
        groups.at(group_id).files.at(filename).seeders.erase(user_addr);
        send_response(sock, "success No longer sharing file.");
        send_sync_message("synced_STOP_SHARE " + group_id + " " + filename + " " + user_addr);
    } else {
        send_response(sock, "error File or group not found.");
    }
}

void Tracker::i_am_seeder(int sock, const vector<string>& args) {
    if (args.size() != 3) return; // i_am_seeder <group_id> <filename>
    string user_id = get_user_id_from_socket(sock);
    if (user_id.empty()) return;

    const string& group_id = args[1];
    const string& filename = args[2];
    string seeder_addr = get_address_from_user_id(user_id);
    if (seeder_addr.empty()) return;

    lock_guard<mutex> lock(groups_mutex);
    if(groups.count(group_id) && groups.at(group_id).files.count(filename)) {
        groups.at(group_id).files.at(filename).seeders.insert(seeder_addr);
        log_msg("User " + user_id + " is now a seeder for " + filename);
        send_sync_message("synced_ADD_SEEDER " + group_id + " " + filename + " " + seeder_addr);
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
    if(logged_in_users.count(user_id)) {
        return logged_in_users.at(user_id);
    }
    return "";
}

// --- Tracker Synchronization Logic ---

void Tracker::listen_for_tracker() {
    int listener_socket = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listener_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in sync_addr;
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

    int sync_sock = accept(listener_socket, nullptr, nullptr);
    close(listener_socket);

    if (sync_sock > 0) {
        log_msg("Other tracker connected for synchronization.");
        {
            lock_guard<mutex> lock(other_tracker_socket_mutex);
            other_tracker_socket = sync_sock;
        }
        handle_sync_connection(sync_sock);
    }
}


void Tracker::connect_to_other_tracker() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(other_tracker_port + 100);
    server_addr.sin_addr.s_addr = inet_addr(other_tracker_addr.c_str());

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        log_msg("Could not connect to other tracker. Will operate in standalone mode.");
        close(sock);
        return;
    }
    
    log_msg("Connected to other tracker.");
    {
        lock_guard<mutex> lock(other_tracker_socket_mutex);
        other_tracker_socket = sock;
    }
    handle_sync_connection(sock);
}

void Tracker::handle_sync_connection(int sync_socket) {
    char buffer[size_piece] = {0};
    while (read(sync_socket, buffer, size_piece) > 0) {
        string sync_command_str(buffer);
        auto args = parse(sync_command_str, " ");
        if (!args.empty()) {
            process_sync_command(args);
        }
        memset(buffer, 0, size_piece);
    }
    log_msg("Connection with other tracker lost.");
    {
        lock_guard<mutex> lock(other_tracker_socket_mutex);
        if (other_tracker_socket == sync_socket) {
            close(other_tracker_socket);
            other_tracker_socket = -1;
        }
    }
}

void Tracker::send_sync_message(const string& message) {
    lock_guard<mutex> lock(other_tracker_socket_mutex);
    if (other_tracker_socket != -1) {
        if(send(other_tracker_socket, message.c_str(), message.length(), 0) < 0) {
            log_msg("Failed to send sync message. Other tracker may be down.");
            close(other_tracker_socket);
            other_tracker_socket = -1;
        } else {
            log_msg("Sent sync message: " + message);
        }
    }
}

void Tracker::process_sync_command(const vector<string>& args) {
    const string& command = args[0];
    log_msg("Received sync command: " + args[0]);

    if (command == "synced_CREATE_USER") {
        lock_guard<mutex> lock(users_mutex);
        users[args[1]] = args[2];
    } else if (command == "synced_LOGIN") {
        lock_guard<mutex> lock(logged_in_users_mutex);
        logged_in_users[args[1]] = args[2];
    } else if (command == "synced_LOGOUT") {
        lock_guard<mutex> lock(logged_in_users_mutex);
        logged_in_users.erase(args[1]);
        lock_guard<mutex> g_lock(groups_mutex);
        for(auto& g_pair : groups) for(auto& f_pair : g_pair.second.files) f_pair.second.seeders.erase(args[2]);
    } else if (command == "synced_CREATE_GROUP") {
        lock_guard<mutex> lock(groups_mutex);
        Group g; g.group_id = args[1]; g.owner_id = args[2]; g.members.insert(args[2]); groups[args[1]] = g;
    } else if (command == "synced_JOIN_GROUP") {
        lock_guard<mutex> lock(groups_mutex);
        if(groups.count(args[1])) groups.at(args[1]).pending_requests.insert(args[2]);
    } else if (command == "synced_LEAVE_GROUP") {
        lock_guard<mutex> lock(groups_mutex);
        if(groups.count(args[1])) groups.at(args[1]).members.erase(args[2]);
    } else if (command == "synced_ACCEPT_REQUEST") {
        lock_guard<mutex> lock(groups_mutex);
        if(groups.count(args[1])) {
            groups.at(args[1]).pending_requests.erase(args[2]);
            groups.at(args[1]).members.insert(args[2]);
        }
    } else if (command == "synced_UPLOAD") {
        lock_guard<mutex> lock(groups_mutex);
        const string& group_id = args[1];
        const string& filename = args[2];
        FileInfo& file = groups[group_id].files[filename];
        file.filename = filename;
        file.file_size = stoll(args[3]);
        file.file_hash = args[4];
        for(size_t i = 5; i < args.size() - 1; ++i) file.piece_hashes[i-5] = args[i];
        file.seeders.insert(args.back());
    } else if (command == "synced_STOP_SHARE") {
        lock_guard<mutex> lock(groups_mutex);
        if(groups.count(args[1]) && groups.at(args[1]).files.count(args[2])) {
            groups.at(args[1]).files.at(args[2]).seeders.erase(args[3]);
        }
    } else if (command == "synced_ADD_SEEDER") {
        lock_guard<mutex> lock(groups_mutex);
        if(groups.count(args[1]) && groups.at(args[1]).files.count(args[2])) {
            groups.at(args[1]).files.at(args[2]).seeders.insert(args[3]);
        }
    }
}

// --- Main Function ---
int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Usage: ./tracker <tracker_info_file> <tracker_no>" << endl;
        return 1;
    }
    string info_file = argv[1];
    int tracker_num = stoi(argv[2]);

    Tracker tracker(info_file, tracker_num);
    tracker.start();

    return 0;
}