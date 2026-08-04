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

const string DATA_DIR = "./data/";
const int INDEX_SIZE = 64;      // padded index length
const int RECORD_SIZE = INDEX_SIZE + 4 + 1; // index(64) + value(4) + flag(1) = 69

struct __attribute__((packed)) Record {
    char index[INDEX_SIZE];
    int32_t value;
    uint8_t flag; // 0=active, 1=deleted (tombstone in log)
};

static string sorted_path() { return DATA_DIR + "sorted.dat"; }
static string log_path()    { return DATA_DIR + "log.dat"; }
static string tmp_path()    { return DATA_DIR + "tmp.dat"; }

static void ensure_dir() { mkdir(DATA_DIR.c_str(), 0755); }

static bool file_exists(const string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static void pad_index(const string& src, char dst[INDEX_SIZE]) {
    size_t len = src.size() < INDEX_SIZE ? src.size() : INDEX_SIZE;
    memcpy(dst, src.c_str(), len);
    if (len < INDEX_SIZE) memset(dst + len, 0, INDEX_SIZE - len);
}

static int cmp_index(const char a[INDEX_SIZE], const char b[INDEX_SIZE]) {
    return memcmp(a, b, INDEX_SIZE);
}

// Read a specific record from sorted file by index
static bool read_sorted_record(FILE* f, int idx, Record& r) {
    if (fseek(f, (long)idx * RECORD_SIZE, SEEK_SET) != 0) return false;
    return fread(&r, RECORD_SIZE, 1, f) == 1;
}

// Get number of records in sorted file
static int sorted_count(FILE* f) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    return (int)(size / RECORD_SIZE);
}

// Binary search in sorted file for first record with given index.
// Returns index of first matching record, or -1 if not found.
static int binary_search_sorted(FILE* f, const char target[INDEX_SIZE], int count) {
    if (count == 0) return -1;
    int lo = 0, hi = count - 1;
    int found = -1;
    Record r;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (!read_sorted_record(f, mid, r)) return -1;
        int c = cmp_index(r.index, target);
        if (c == 0) {
            found = mid;
            hi = mid - 1; // look for first occurrence
        } else if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return found;
}

// Record comparator for sorting
static bool record_less(const Record& a, const Record& b) {
    int c = cmp_index(a.index, b.index);
    if (c != 0) return c < 0;
    return a.value < b.value;
}

// Read all records from log file
static void read_log(vector<Record>& out) {
    string lp = log_path();
    if (!file_exists(lp)) return;
    ifstream in(lp, ios::binary);
    Record r;
    while (in.read(reinterpret_cast<char*>(&r), RECORD_SIZE)) {
        out.push_back(r);
    }
}

// Merge log into sorted file - simplified, correct version
static void merge() {
    string lp = log_path();
    if (!file_exists(lp)) return;

    // Check if log is large enough to justify merge
    struct stat st;
    if (stat(lp.c_str(), &st) != 0) return;
    if (st.st_size < 20 * 1024) return; // only merge when log > 20KB

    string sp = sorted_path();

    // Read all sorted records
    vector<Record> all;
    if (file_exists(sp)) {
        ifstream sin(sp, ios::binary);
        Record r;
        while (sin.read(reinterpret_cast<char*>(&r), RECORD_SIZE)) {
            all.push_back(r);
        }
    }

    // Read log records and apply them
    ifstream lin(lp, ios::binary);
    Record lr;
    while (lin.read(reinterpret_cast<char*>(&lr), RECORD_SIZE)) {
        if (lr.flag == 0) {
            // Insert: add to all (no duplicate check needed, input guarantees)
            all.push_back(lr);
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
    lin.close();

    // Sort all records
    sort(all.begin(), all.end(), record_less);

    // Write to sorted file
    ofstream sout(sp, ios::binary | ios::trunc);
    for (auto& r : all) r.flag = 0;
    if (!all.empty()) {
        sout.write(reinterpret_cast<const char*>(all.data()), all.size() * RECORD_SIZE);
    }
    sout.close();

    // Truncate log file
    ofstream lclear(lp, ios::binary | ios::trunc);
    lclear.close();
}

static void do_insert(const string& index, int value) {
    string lp = log_path();
    Record r;
    pad_index(index, r.index);
    r.value = value;
    r.flag = 0; // insert

    ofstream out(lp, ios::binary | ios::app);
    out.write(reinterpret_cast<const char*>(&r), RECORD_SIZE);
    out.close();
}

static void do_delete(const string& index, int value) {
    string lp = log_path();
    Record r;
    pad_index(index, r.index);
    r.value = value;
    r.flag = 1; // delete

    ofstream out(lp, ios::binary | ios::app);
    out.write(reinterpret_cast<const char*>(&r), RECORD_SIZE);
    out.close();
}

static void do_find(const string& index) {
    char target[INDEX_SIZE];
    pad_index(index, target);

    // Binary search sorted file
    vector<int> values;

    string sp = sorted_path();
    FILE* f = fopen(sp.c_str(), "rb");
    if (f) {
        int count = sorted_count(f);
        int first = binary_search_sorted(f, target, count);
        if (first >= 0) {
            // Scan forward for all matching records
            Record r;
            for (int i = first; i < count; i++) {
                if (!read_sorted_record(f, i, r)) break;
                if (cmp_index(r.index, target) != 0) break;
                values.push_back(r.value);
            }
        }
        fclose(f);
    }

    // Apply log operations
    string lp = log_path();
    if (file_exists(lp)) {
        ifstream in(lp, ios::binary);
        Record r;
        while (in.read(reinterpret_cast<char*>(&r), RECORD_SIZE)) {
            if (cmp_index(r.index, target) != 0) continue;
            if (r.flag == 0) {
                values.push_back(r.value);
            } else {
                auto it = find(values.begin(), values.end(), r.value);
                if (it != values.end()) values.erase(it);
            }
        }
    }

    // Sort and output
    sort(values.begin(), values.end());

    if (values.empty()) {
        cout << "null\n";
    } else {
        for (size_t i = 0; i < values.size(); i++) {
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
    cin.ignore();

    string line;
    for (int i = 0; i < n; i++) {
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

        // Periodically try to merge (every 500 operations)
        if (i % 500 == 499) {
            merge();
        }
    }

    // Final merge to ensure all data is compacted
    merge();

    cout.flush();
    return 0;
}