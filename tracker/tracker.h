#ifndef TRACKER_H
#define TRACKER_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>

using namespace std;

struct FileInfo {
    string filename;
    long long file_size;
    string file_hash;
    map<int, string> piece_hashes;
    set<string> seeders; // client_ip:port
};

struct Group {
    string group_id;
    string owner_id;
    set<string> members;
    set<string> pending_requests;
    map<string, FileInfo> files;
};

class Tracker {
public:
    Tracker(const string& info_file, int tracker_num);
    void start();

private:
    void listen_for_clients();
    void handle_client(int client_socket, const string& client_addr);
    void listen_for_tracker();
    void handle_sync_connection(int sync_socket);
    void connect_to_other_tracker();
    void process_command(int client_socket, const string& client_addr, const vector<string>& args);
    void process_sync_command(const vector<string>& args);
    void send_sync_message(const string& message);

    // Command handlers
    void create_user(int sock, const vector<string>& args);
    void login(int sock, const string& client_addr, const vector<string>& args);
    void logout(int sock, const vector<string>& args);
    void create_group(int sock, const vector<string>& args);
    void join_group(int sock, const vector<string>& args);
    void leave_group(int sock, const vector<string>& args);
    void list_requests(int sock, const vector<string>& args);
    void accept_request(int sock, const vector<string>& args);
    void list_groups(int sock);
    void list_files(int sock, const vector<string>& args);
    void upload_file(int sock, const vector<string>& args);
    void download_file(int sock, const vector<string>& args);
    void stop_share(int sock, const vector<string>& args);
    void i_am_seeder(int sock, const vector<string>& args); // New command for completed downloads

    // Helper methods
    string get_user_id_from_socket(int sock);
    string get_address_from_user_id(const string& user_id);
    
    // Server state
    string ip_addr;
    int port;
    string other_tracker_addr;
    int other_tracker_port;
    int tracker_id;
    int server_socket;

    // Synchronized data
    map<string, string> users; // user_id -> password
    map<string, string> logged_in_users; // user_id -> client_ip:port
    map<int, string> socket_to_user; // client_socket -> user_id
    map<string, Group> groups;

    // Mutexes
    mutex users_mutex;
    mutex logged_in_users_mutex;
    mutex groups_mutex;
    mutex socket_to_user_mutex;
    mutex other_tracker_socket_mutex;
    int other_tracker_socket = -1;
};

#endif // TRACKER_H