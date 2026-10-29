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
