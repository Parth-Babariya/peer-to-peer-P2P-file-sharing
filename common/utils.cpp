#include "utils.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <cctype>
#include <cstdlib>

using namespace std;

static string to_hex(const unsigned char* bytes, unsigned int len) {
    stringstream ss;
    ss << hex << setfill('0');
    for (unsigned int i = 0; i < len; ++i) {
        ss << setw(2) << static_cast<unsigned int>(bytes[i]);
    }
    return ss.str();
}

Sha1::Sha1() : ctx(EVP_MD_CTX_new()) {
    EVP_DigestInit_ex(static_cast<EVP_MD_CTX*>(ctx), EVP_sha1(), nullptr);
}

Sha1::~Sha1() {
    EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx));
}

void Sha1::update(const char* data, size_t len) {
    EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx), data, len);
}

string Sha1::hex() {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx), digest, &len);
    return to_hex(digest, len);
}

string sha(const char* data, size_t len) {
    Sha1 h;
    h.update(data, len);
    return h.hex();
}

string sha_file(const string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";
    Sha1 h;
    vector<char> buf(PIECE_SIZE);
    ssize_t n;
    while ((n = read(fd, buf.data(), buf.size())) > 0) {
        h.update(buf.data(), n);
    }
    close(fd);
    return n < 0 ? "" : h.hex();
}

vector<string> parse(const string& str, const string& delimiter) {
    vector<string> tokens;
    size_t start = 0, end = 0;
    while ((end = str.find(delimiter, start)) != string::npos) {
        tokens.push_back(str.substr(start, end - start));
        start = end + delimiter.length();
    }
    tokens.push_back(str.substr(start));
    return tokens;
}

string encode_field(const string& field) {
    static const char* digits = "0123456789ABCDEF";
    string out;
    for (unsigned char c : field) {
        if (c == '%' || c == ' ' || c < 0x20 || c == 0x7f) {
            out += '%';
            out += digits[c >> 4];
            out += digits[c & 15];
        } else {
            out += c;
        }
    }
    return out;
}

string make_message(const vector<string>& fields) {
    string msg;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) msg += ' ';
        msg += encode_field(fields[i]);
    }
    return msg;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_field(const string& s, string& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        int hi = i + 2 < s.size() ? hex_value(s[i + 1]) : -1;
        int lo = i + 2 < s.size() ? hex_value(s[i + 2]) : -1;
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    return true;
}

bool decode_fields(const string& msg, vector<string>& fields) {
    fields.clear();
    for (const auto& raw : parse(msg, " ")) {
        string field;
        if (raw.empty() || !decode_field(raw, field)) {
            fields.clear();
            return false;
        }
        fields.push_back(field);
    }
    return true;
}

bool split_words(const string& line, vector<string>& words) {
    words.clear();
    size_t i = 0;
    while (true) {
        while (i < line.size() && isspace(static_cast<unsigned char>(line[i]))) i++;
        if (i == line.size()) return true;
        string word;
        bool quoted = false;
        while (i < line.size() && (quoted || !isspace(static_cast<unsigned char>(line[i])))) {
            if (line[i] == '"') {
                quoted = !quoted;
            } else {
                word += line[i];
            }
            i++;
        }
        if (quoted || word.empty()) {
            words.clear();
            return false;
        }
        words.push_back(word);
    }
}

string random_hex(size_t bytes) {
    vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), bytes) != 1) {
        log_msg("FATAL: no randomness available.");
        abort();
    }
    return to_hex(buf.data(), bytes);
}

bool same_secret(const string& a, const string& b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// OWASP suggests 600,000 for PBKDF2-SHA256; this is lower to keep logins (and
// the tests) quick. Each record stores its own count, so it can be raised later.
static const int PASSWORD_HASH_ITERATIONS = 100000;

static string pbkdf2(const string& password, const string& salt, int iterations) {
    unsigned char out[32];
    PKCS5_PBKDF2_HMAC(password.data(), password.size(),
                      reinterpret_cast<const unsigned char*>(salt.data()), salt.size(),
                      iterations, EVP_sha256(), sizeof(out), out);
    return to_hex(out, sizeof(out));
}

string hash_password(const string& password) {
    string salt = random_hex(16);
    return "pbkdf2_sha256$" + to_string(PASSWORD_HASH_ITERATIONS) + "$" + salt + "$" +
           pbkdf2(password, salt, PASSWORD_HASH_ITERATIONS);
}

bool check_password(const string& password, const string& stored) {
    auto parts = parse(stored, "$");
    if (parts.size() != 4 || parts[0] != "pbkdf2_sha256") return false;
    int iterations;
    try {
        iterations = stoi(parts[1]);
    } catch (const exception&) {
        return false;
    }
    if (iterations <= 0) return false;
    return same_secret(pbkdf2(password, parts[2], iterations), parts[3]);
}

void log_msg(const string& msg) {
    static mutex log_mutex;
    lock_guard<mutex> lock(log_mutex);
    cout << "[log] " << msg << endl;
}

// send() and read() may move fewer bytes than asked; these loop until all of them are done.
bool send_all(int sock, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = send(sock, data, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        len -= n;
    }
    return true;
}

static bool recv_all(int sock, char* data, size_t len) {
    while (len > 0) {
        ssize_t n = read(sock, data, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        len -= n;
    }
    return true;
}

bool send_msg(int sock, const string& msg) {
    if (msg.size() > MAX_MSG_SIZE) return false;
    uint32_t len = htonl(static_cast<uint32_t>(msg.size()));
    return send_all(sock, reinterpret_cast<const char*>(&len), sizeof(len)) &&
           send_all(sock, msg.data(), msg.size());
}

bool recv_msg(int sock, string& msg) {
    uint32_t len;
    if (!recv_all(sock, reinterpret_cast<char*>(&len), sizeof(len))) return false;
    len = ntohl(len);
    if (len > MAX_MSG_SIZE) return false;
    msg.assign(len, '\0');
    return len == 0 || recv_all(sock, &msg[0], len);
}
