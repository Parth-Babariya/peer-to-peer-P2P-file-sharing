#ifndef CLIENT_H
#define CLIENT_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>

using namespace std;

const int PIECE_SIZE = 512 * 1024; // 512KB

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

class Client {
public:
    Client(const string& tracker_info_file);
    void run();

private:
    void start_seeder_service();
    void handle_peer_connection(int peer_socket);
    void process_user_input();
    
    // connection failover management
    bool connect_to_available_tracker();
    bool try_connect_to(const string& addr);
    string send_to_tracker(const string& command, bool is_retry = false);

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
    
    int seeder_port;

    bool is_logged_in = false;
    string user_id;
    string password;

    map<string, DownloadState> ongoing_downloads; // filename -> state
    mutex downloads_mutex;

    map<string, string> shared_files; // filename -> local_path
    mutex shared_files_mutex;
};

#endif // CLIENT_H