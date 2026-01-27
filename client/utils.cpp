#include "utils.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <openssl/sha.h> // Reverted to the required SHA1 header

using namespace std;

// Reverted to the one-shot SHA1 function as required
string sha(const char* data, size_t len) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(data), len, hash);
    
    stringstream ss;
    ss << hex << setfill('0');
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) {
        ss << setw(2) << static_cast<unsigned int>(hash[i]);
    }
    return ss.str();
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
    cout << "[log] " << msg << endl;
}
