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

// A message is a list of fields separated by single spaces. A field may itself
// contain spaces (a file name such as "my notes.txt"), so on the wire '%', spaces
// and control characters are percent-encoded: "my%20notes.txt".
string encode_field(const string& field);
// Encodes each field and joins them into one message.
string make_message(const vector<string>& fields);
// Splits a message and decodes each field. False (and no fields) on an empty
// field or a malformed escape.
bool decode_fields(const string& msg, vector<string>& fields);

// Splits a line typed at the prompt into words; "double quotes" keep spaces
// inside one word. False on an unmatched quote or an empty quoted word.
bool split_words(const string& line, vector<string>& words);

// `bytes` random bytes from OpenSSL's CSPRNG, as hex.
string random_hex(size_t bytes);

// Compares two secrets in time that doesn't depend on where they first differ.
bool same_secret(const string& a, const string& b);

// What the tracker stores instead of a password: a salted, deliberately slow
// PBKDF2-SHA256 hash, "pbkdf2_sha256$<iterations>$<salt>$<hash>".
string hash_password(const string& password);
bool check_password(const string& password, const string& stored);

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
