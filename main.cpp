#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;

const int NUM_BUCKETS = 19;
const string DATA_DIR = "./data/";

// DJB2 hash
static uint32_t hash_str(const string& s) {
    uint32_t h = 5381;
    for (unsigned char c : s) h = ((h << 5) + h) + c;
    return h;
}

static int get_bucket(const string& s) {
    return hash_str(s) % NUM_BUCKETS;
}

static string bucket_path(int b) {
    return DATA_DIR + to_string(b) + ".dat";
}

static void ensure_dir() {
    mkdir(DATA_DIR.c_str(), 0755);
}

static bool file_exists(const string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void do_insert(const string& index, int value) {
    int b = get_bucket(index);
    string path = bucket_path(b);

    // Append new entry (input guarantees no duplicate index,value pairs)
    ofstream out(path, ios::binary | ios::app);
    uint8_t len = static_cast<uint8_t>(index.size());
    out.write(reinterpret_cast<const char*>(&len), 1);
    out.write(index.c_str(), len);
    out.write(reinterpret_cast<const char*>(&value), 4);
}

static void do_delete(const string& index, int value) {
    int b = get_bucket(index);
    string path = bucket_path(b);

    if (!file_exists(path)) return;

    string tmp_path = path + ".tmp";

    ifstream in(path, ios::binary);
    ofstream out(tmp_path, ios::binary);

    while (in) {
        uint8_t len;
        in.read(reinterpret_cast<char*>(&len), 1);
        if (!in) break;

        string idx(len, '\0');
        in.read(&idx[0], len);
        if (!in) break;

        int val;
        in.read(reinterpret_cast<char*>(&val), 4);
        if (!in) break;

        if (idx == index && val == value) {
            continue; // skip deleted entry
        }

        out.write(reinterpret_cast<const char*>(&len), 1);
        out.write(idx.c_str(), len);
        out.write(reinterpret_cast<const char*>(&val), 4);
    }

    in.close();
    out.close();

    rename(tmp_path.c_str(), path.c_str());
}

static void do_find(const string& index) {
    int b = get_bucket(index);
    string path = bucket_path(b);

    vector<int> values;

    if (file_exists(path)) {
        ifstream in(path, ios::binary);
        while (in) {
            uint8_t len;
            in.read(reinterpret_cast<char*>(&len), 1);
            if (!in) break;

            string idx(len, '\0');
            in.read(&idx[0], len);
            if (!in) break;

            int val;
            in.read(reinterpret_cast<char*>(&val), 4);
            if (!in) break;

            if (idx == index) {
                values.push_back(val);
            }
        }
    }

    if (values.empty()) {
        cout << "null\n";
    } else {
        sort(values.begin(), values.end());
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) cout << ' ';
            cout << values[i];
        }
        cout << '\n';
    }
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    ensure_dir();

    int n;
    cin >> n;
    cin.ignore(); // consume newline after n

    string line;
    for (int i = 0; i < n; ++i) {
        if (!getline(cin, line)) break;

        size_t space1 = line.find(' ');
        string cmd = line.substr(0, space1);

        if (cmd == "insert") {
            size_t space2 = line.find(' ', space1 + 1);
            string index = line.substr(space1 + 1, space2 - space1 - 1);
            int value = stoi(line.substr(space2 + 1));
            do_insert(index, value);
        } else if (cmd == "delete") {
            size_t space2 = line.find(' ', space1 + 1);
            string index = line.substr(space1 + 1, space2 - space1 - 1);
            int value = stoi(line.substr(space2 + 1));
            do_delete(index, value);
        } else if (cmd == "find") {
            string index = line.substr(space1 + 1);
            do_find(index);
        }
    }
    cout.flush();
    return 0;
}