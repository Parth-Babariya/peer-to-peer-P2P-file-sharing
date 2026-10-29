#ifndef CLIENT_H
#define CLIENT_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <utility>
#include <atomic>

using namespace std;

struct DownloadState {
    string group_id;
    string filename;
    string destination_path;
    long long file_size;
    int total_pieces;
    vector<bool> pieces_downloaded;
    string status; // "Downloading", "Completed", "Failed"
    map<int, string> piece_hashes;
};

struct SharedFile {
    string path;
    string key; // share key from the tracker; peers must present it
};

class Client {
public:
    // upload_limit_kbps: cap on bytes served to peers (0 = no cap), to emulate a home uplink.
    // partial_sharing: serve pieces of a file while still downloading it.
    Client(const string& tracker_info_file, long long upload_limit_kbps, bool partial_sharing);
    void run();

private:
    int open_seeder_socket();
    void start_seeder_service(int seeder_socket);
    void handle_peer_connection(int peer_socket);
    void send_paced(int sock, const string& data);
    void pace(size_t bytes);
    void process_user_input();
    
    // connection failover management
    bool connect_to_available_tracker();
    bool try_connect_to(const string& addr);
    string send_to_tracker(const string& command);
    string reconnect();
    bool request_reply(const string& command, string& response);

    // command handlers
    void handle_upload(const vector<string>& args);
    void handle_download(const vector<string>& args);
    void show_downloads();
    void handle_login(const vector<string>& args);
    
    void download_manager(const string& group_id, const string& filename, const string& dest_path, const vector<string>& metadata);

    // tracker state variables
    vector<string> tracker_addresses;
    int current_tracker_idx = 0;
    int tracker_socket = -1;
    // The prompt and every download thread share one tracker connection; each
    // request and its reply happen under this lock so replies can't be swapped.
    mutex tracker_mutex;

    int seeder_port;

    bool is_logged_in = false;
    string user_id;
    string password;

    // Both keyed by (group, file name): two groups may each have a file of the same name.
    map<pair<string, string>, DownloadState> ongoing_downloads;
    mutex downloads_mutex;

    map<pair<string, string>, SharedFile> shared_files;
    mutex shared_files_mutex;

    bool partial_sharing;
    long long upload_limit_bytes; // per second, 0 = unlimited
    mutex pace_mutex;
    atomic<int> active_uploads{0}; // pieces being sent right now, at most UPLOAD_SLOTS
chrono::steady_clock::time_point pace_next;
};

#endif // CLIENT_H