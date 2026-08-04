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

const int NUM_BUCKETS = 9;
const string DATA_DIR = "./data/";
const int INDEX_SIZE = 64;      // padded index length
const int VAL_SIZE = 4;         // int32 value
const int FLAG_SIZE = 1;        // 0=insert(add), 1=delete(tombstone)
const int RECORD_SIZE = INDEX_SIZE + VAL_SIZE + FLAG_SIZE; // 69 bytes
const size_t LOG_MERGE_THRESHOLD = 50 * 1024; // merge log when > 50KB

struct Record {
    char index[INDEX_SIZE];
    int value;
    uint8_t flag; // 0 for insert/add, 1 for delete/tombstone
};

// DJB2 hash
static uint32_t hash_str(const string& s) {
    uint32_t h = 5381;
    for (unsigned char c : s) h = ((h << 5) + h) + c;
    return h;
}

static int get_bucket(const string& s) {
    return hash_str(s) % NUM_BUCKETS;
}

static string sorted_path(int b) {
    return DATA_DIR + to_string(b) + "_s.dat";
}

static string log_path(int b) {
    return DATA_DIR + to_string(b) + "_l.dat";
}

static void ensure_dir() {
    mkdir(DATA_DIR.c_str(), 0755);
}

static bool file_exists(const string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void pad_index(const string& src, char dst[INDEX_SIZE]) {
    size_t len = min(src.size(), (size_t)INDEX_SIZE);
    memcpy(dst, src.c_str(), len);
    if (len < INDEX_SIZE) memset(dst + len, 0, INDEX_SIZE - len);
}

// Read all records from a file into vector
static void read_records(const string& path, vector<Record>& out) {
    if (!file_exists(path)) return;
    ifstream in(path, ios::binary);
    Record r;
    while (in.read(reinterpret_cast<char*>(&r), RECORD_SIZE)) {
        out.push_back(r);
    }
}

// Write all records to a file
static void write_records(const string& path, const vector<Record>& recs) {
    ofstream out(path, ios::binary | ios::trunc);
    out.write(reinterpret_cast<const char*>(recs.data()), recs.size() * RECORD_SIZE);
}

// Compare two padded indices
static int cmp_index(const char a[INDEX_SIZE], const char b[INDEX_SIZE]) {
    return memcmp(a, b, INDEX_SIZE);
}

// Find records in sorted vector matching target index
static void find_in_sorted(const vector<Record>& sorted, const char target[INDEX_SIZE],
                           vector<int>& values) {
    if (sorted.empty()) return;

    // Binary search for first occurrence
    auto it = lower_bound(sorted.begin(), sorted.end(), target,
        [](const Record& r, const char t[INDEX_SIZE]) {
            return cmp_index(r.index, t) < 0;
        });

    while (it != sorted.end() && cmp_index(it->index, target) == 0) {
        values.push_back(it->value);
        ++it;
    }
}

// Apply log operations to a value set
static void apply_log(const vector<Record>& log, const char target[INDEX_SIZE],
                      vector<int>& values) {
    for (const auto& r : log) {
        if (cmp_index(r.index, target) != 0) continue;
        if (r.flag == 0) {
            // Insert: add value (assume no duplicate)
            values.push_back(r.value);
        } else {
            // Delete: remove value
            auto it = find(values.begin(), values.end(), r.value);
            if (it != values.end()) values.erase(it);
        }
    }
}

// Merge log into sorted file for a bucket
static void merge_bucket(int b) {
    string sp = sorted_path(b);
    string lp = log_path(b);

    if (!file_exists(lp)) return;
    // Check log size
    struct stat st;
    if (stat(lp.c_str(), &st) != 0) return;
    if ((size_t)st.st_size <= LOG_MERGE_THRESHOLD) return;

    vector<Record> sorted_recs;
    vector<Record> log_recs;

    read_records(sp, sorted_recs);
    read_records(lp, log_recs);

    // Build a map of existing entries (index,value) from sorted
    // Then apply log operations
    // We'll use a vector and sort after applying

    // Convert sorted to a set for easy lookup/deletion
    // Key: padded index (64 bytes) + value (4 bytes) = 68 bytes
    // We'll use a map<string, int> - no, too much memory.

    // Instead, process entries sequentially:
    // 1. Collect all entries from sorted as (index_padded, value) pairs
    // 2. Apply log: for each insert, add pair; for each delete, remove pair
    // 3. Sort all remaining pairs
    // 4. Write back

    // For simplicity, use a vector<Record> and process
    vector<Record> all = sorted_recs;

    for (const auto& lr : log_recs) {
        if (lr.flag == 0) {
            // Insert
            all.push_back(lr);
            all.back().flag = 0;
        } else {
            // Delete: find and remove
            for (auto it = all.begin(); it != all.end(); ++it) {
                if (cmp_index(it->index, lr.index) == 0 && it->value == lr.value) {
                    all.erase(it);
                    break;
                }
            }
        }
    }

    // Sort all by (index, value)
    sort(all.begin(), all.end(), [](const Record& a, const Record& b) {
        int c = cmp_index(a.index, b.index);
        if (c != 0) return c < 0;
        return a.value < b.value;
    });

    // Write back to sorted file
    write_records(sp, all);

    // Truncate log file
    ofstream trunc(lp, ios::binary | ios::trunc);
    trunc.close();
}

static void do_insert(const string& index, int value) {
    int b = get_bucket(index);
    string lp = log_path(b);

    Record r;
    pad_index(index, r.index);
    r.value = value;
    r.flag = 0;

    ofstream out(lp, ios::binary | ios::app);
    out.write(reinterpret_cast<const char*>(&r), RECORD_SIZE);
    out.close();

    // Check if we should merge
    merge_bucket(b);
}

static void do_delete(const string& index, int value) {
    int b = get_bucket(index);
    string lp = log_path(b);

    Record r;
    pad_index(index, r.index);
    r.value = value;
    r.flag = 1;

    ofstream out(lp, ios::binary | ios::app);
    out.write(reinterpret_cast<const char*>(&r), RECORD_SIZE);
    out.close();

    // Check if we should merge
    merge_bucket(b);
}

static void do_find(const string& index) {
    char target[INDEX_SIZE];
    pad_index(index, target);

    int b = get_bucket(index);

    // Read sorted file
    vector<Record> sorted_recs;
    read_records(sorted_path(b), sorted_recs);

    // Read log file
    vector<Record> log_recs;
    read_records(log_path(b), log_recs);

    // Get values from sorted
    vector<int> values;
    find_in_sorted(sorted_recs, target, values);

    // Apply log operations
    apply_log(log_recs, target, values);

    // Sort and deduplicate (in case of re-insert)
    sort(values.begin(), values.end());

    if (values.empty()) {
        cout << "null\n";
    } else {
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