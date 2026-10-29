#ifndef LWW_H
#define LWW_H

// Last-writer-wins state, so two trackers that each kept changing things while
// cut off from each other end up identical once they reconnect.
//
// Every change carries a Stamp. Each tracker applies every change it hears of,
// its own and the other's, and for each key the change with the highest stamp
// wins, whatever order they arrive in. A removal is kept as a tombstone instead
// of being erased, so the other side's older "present" can't bring it back.

#include <string>
#include <map>
#include <vector>
#include <tuple>
#include <algorithm>
#include <cctype>

using namespace std;

struct Stamp {
    unsigned long long counter = 0; // Lamport clock of the tracker that made the change
    int tracker = 0;                // breaks ties, so both trackers pick the same winner

    bool operator<(const Stamp& o) const { return tie(counter, tracker) < tie(o.counter, o.tracker); }

    // "<counter>.<tracker>"
    string str() const { return to_string(counter) + "." + to_string(tracker); }

    static bool parse(const string& s, Stamp& out) {
        size_t dot = s.find('.');
        if (dot == string::npos || dot == 0 || dot > 19 || s.size() - dot - 1 == 0 || s.size() - dot - 1 > 9) {
            return false;
        }
        auto digits = [](const string& part) { return all_of(part.begin(), part.end(), ::isdigit); };
        string counter = s.substr(0, dot), tracker = s.substr(dot + 1);
        if (!digits(counter) || !digits(tracker)) return false;
        out.counter = stoull(counter);
        out.tracker = stoi(tracker);
        return true;
    }
};

struct LwwEntry {
    bool present = false;
    string value;
    Stamp stamp;
};

// A map, or with empty values a set, in which the newest change to each key wins.
class LwwMap {
public:
    // Applies one change. Ignored if the key already has a change at least as new;
    // an equal stamp is the same change heard twice.
    void apply(const string& key, bool present, const string& value, const Stamp& stamp) {
        auto it = entries_.find(key);
        if (it != entries_.end() && !(it->second.stamp < stamp)) return;
        entries_[key] = {present, value, stamp};
    }

    bool contains(const string& key) const {
        auto it = entries_.find(key);
        return it != entries_.end() && it->second.present;
    }

    // The value of a present key, or "".
    string value(const string& key) const {
        return contains(key) ? entries_.at(key).value : "";
    }

    // Present keys, in order.
    vector<string> keys() const {
        vector<string> out;
        for (const auto& [key, entry] : entries_) {
            if (entry.present) out.push_back(key);
        }
        return out;
    }

    bool empty() const { return keys().empty(); }

    // Every key, tombstones included: what a snapshot must carry.
    const map<string, LwwEntry>& entries() const { return entries_; }

private:
    map<string, LwwEntry> entries_;
};

#endif // LWW_H
