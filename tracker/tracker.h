#ifndef TRACKER_H
#define TRACKER_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>
#include "lww.h"

using namespace std;

struct FileInfo {
    string filename;
    long long file_size;
    string file_hash;
    // Random secret the uploader picked. Peers serve pieces only to whoever
    // presents it, and the tracker hands it out only to group members.
    string share_key;
    map<int, string> piece_hashes;
    Stamp stamp;    // of the fields above; a re-upload replaces them
    LwwMap seeders; // client_ip:port
};

struct Group {
    string group_id;
    string owner_id;
    Stamp owner_stamp;
    LwwMap members;
    LwwMap pending_requests;
    map<string, FileInfo> files;
};

class Tracker {
public:
    // state_file: where this tracker keeps its state across restarts.
    Tracker(const string& info_file, int tracker_num, const string& state_file);
    void start();

private:
    void listen_for_clients();
    void handle_client(int client_socket, const string& client_addr);
    void listen_for_tracker();
    void handle_sync_connection(int sync_socket);
    void connect_to_other_tracker();
    void run_sync_session(int sync_socket);
    void send_snapshot_and_attach(int sync_socket);
    void process_command(int client_socket, const string& client_addr, vector<string> args);
    void dispatch_command(int client_socket, const string& client_addr, const vector<string>& args);
    void process_sync_command(const vector<string>& args);
    void apply_sync_command(const vector<string>& args);
    void send_sync_message(const string& message);
    void wait_to_serve_alone();
    vector<string> state_messages();
    void load_state();
    void persist(const string& message);
Stamp next_stamp();
    void observe(const Stamp& stamp);
    void set_member(Group& group, const string& user_id, bool present, const Stamp& stamp);
    void set_pending(Group& group, const string& user_id, bool present, const Stamp& stamp);
    void set_seeder(Group& group, FileInfo& file, const string& addr, bool present, const Stamp& stamp);

    // Command handlers
    void create_user(int sock, const vector<string>& args);
    void login(int sock, const string& client_addr, const vector<string>& args);
    void logout(int sock, const vector<string>& args);
    void end_session(int sock, const string& user_id);
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
    void i_am_seeder(int sock, const vector<string>& args); // a finished download now serves the file

    // Helper methods
    string get_user_id_from_socket(int sock);
    string get_address_from_user_id(const string& user_id);
    bool find_result(const string& request_id, string& reply);
    void remember_result(const string& request_id, const string& reply);
    
    // Server state
    string ip_addr;
    int port;
    string other_tracker_addr;
    int other_tracker_port;
    int tracker_id;
    int server_socket;

    // Synchronized data. Everything the other tracker also holds is last-writer-wins
    // (lww.h), and every change to it is synced as one stamped state change.
    LwwMap users; // user_id -> hash_password() record, never the password
    LwwMap logged_in_users; // user_id -> client_ip:port
    map<int, string> socket_to_user; // client_socket -> user_id (this tracker's connections only)
    map<string, Group> groups;
    // Lamport clock: above every stamp this tracker has made or seen, so each new
    // change outranks everything it could have known about.
    atomic<unsigned long long> clock{0};

    // Mutexes. When holding more than one, always take them in this order:
    //   users -> logged_in_users -> socket_to_user -> groups -> other_tracker_socket
    // state_file_mutex comes after groups and is never held with other_tracker_socket.
    // Taking them in different orders on two threads can deadlock.
    mutex users_mutex;
    mutex logged_in_users_mutex;
    mutex groups_mutex;
    mutex socket_to_user_mutex;
    mutex other_tracker_socket_mutex;
    int other_tracker_socket = -1;

    // False until this tracker holds the other one's state (its snapshot has been
    // applied), or the other one stayed unreachable for a grace period. Until then
    // clients are turned away: an empty, restarted tracker must not answer for both.
    atomic<bool> caught_up{false};
    atomic<bool> sync_session_active{false};
    // Cleared by the console's "disconnect" (to simulate a network split) and set
    // again by "reconnect".
    atomic<bool> sync_enabled{true};

    // Replies to recent state-changing commands, by request id, on both trackers:
    // a command retried after failover gets its first reply instead of running
    // twice. Oldest dropped first. results_mutex is never held with another lock.
    map<string, string> results;
    deque<string> results_order;
    mutex results_mutex;

    // Every state change, this tracker's and the other's, appended as its sync
    // message and flushed to disk. Replayed and compacted at startup, so the
    // state survives both trackers going down.
    string state_file;
    int state_fd = -1;
    mutex state_file_mutex;
};

#endif // TRACKER_H