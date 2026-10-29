#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>

using namespace std;

const int PIECE_SIZE = 512 * 1024; // 512KB

// Largest message either side accepts; a bigger length prefix means a broken or hostile peer.
const size_t MAX_MSG_SIZE = 64 * 1024 * 1024;

// SHA1 of a buffer, as 40 hex characters
string sha(const char* data, size_t len);

// Incremental SHA1, for hashing a whole file while reading it piece by piece
class Sha1 {
public:
    Sha1();
    ~Sha1();
    Sha1(const Sha1&) = delete;
    Sha1& operator=(const Sha1&) = delete;
    void update(const char* data, size_t len);
    string hex();
private:
    void* ctx;
};

// SHA1 of a file's contents, or "" if it cannot be read
string sha_file(const string& path);

// Function to split a string by a delimiter
vector<string> parse(const string& str, const string& delimiter);

// Log message to console with a prefix (safe to call from several threads)
void log_msg(const string& msg);

// TCP is a byte stream, so every message on every socket is framed:
// a 4-byte big-endian length followed by exactly that many bytes.
// Both return false if the connection is closed or broken.
bool send_msg(int sock, const string& msg);
bool recv_msg(int sock, string& msg);

// Sends exactly len bytes, looping over partial sends.
bool send_all(int sock, const char* data, size_t len);

#endif // UTILS_H
