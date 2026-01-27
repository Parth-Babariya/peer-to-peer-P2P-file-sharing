#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
#include <openssl/sha.h>

using namespace std;

// Function to calculate SHA1 hash of data
string sha(const char* data, size_t len);

// Function to split a string by a delimiter
vector<string> parse(const string& str, const string& delimiter);

// Log message to console with a prefix
void log_msg(const string& msg);

#endif // UTILS_H