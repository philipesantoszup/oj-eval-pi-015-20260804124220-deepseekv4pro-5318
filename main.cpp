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
const int INDEX_SIZE = 64;
const int RECORD_SIZE = INDEX_SIZE + 4 + 1; // 69

struct __attribute__((packed)) Record {
    char index[INDEX_SIZE];
    int32_t value;
    uint8_t flag;
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

static bool record_less(const Record& a, const Record& b) {
    int c = cmp_index(a.index, b.index);
    if (c != 0) return c < 0;
    return a.value < b.value;
}

static bool read_sorted_record(FILE* f, int idx, Record& r) {
    if (fseek(f, (long)idx * RECORD_SIZE, SEEK_SET) != 0) return false;
    return fread(&r, RECORD_SIZE, 1, f) == 1;
}

static int sorted_count(FILE* f) {
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    return (int)(size / RECORD_SIZE);
}

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
            hi = mid - 1;
        } else if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return found;
}

static void merge() {
    string lp = log_path();
    if (!file_exists(lp)) return;

    struct stat st;
    if (stat(lp.c_str(), &st) != 0) return;
    if (st.st_size < 20 * 1024) return;

    string sp = sorted_path();

    // Read all log records
    vector<Record> log_recs;
    {
        ifstream lin(lp, ios::binary);
        Record r;
        while (lin.read(reinterpret_cast<char*>(&r), RECORD_SIZE)) {
            log_recs.push_back(r);
        }
    }

    // Process log chronologically: build net insertions and removals
    vector<Record> inserts;
    vector<Record> removals;

    for (const auto& lr : log_recs) {
        if (lr.flag == 0) {
            // Insert: cancel any matching removal, then add
            for (auto it = removals.begin(); it != removals.end(); ++it) {
                if (cmp_index(it->index, lr.index) == 0 && it->value == lr.value) {
                    removals.erase(it);
                    break;
                }
            }
            inserts.push_back(lr);
        } else {
            // Delete: remove from inserts if present, else add to removals
            bool found = false;
            for (auto it = inserts.begin(); it != inserts.end(); ++it) {
                if (cmp_index(it->index, lr.index) == 0 && it->value == lr.value) {
                    inserts.erase(it);
                    found = true;
                    break;
                }
            }
            if (!found) removals.push_back(lr);
        }
    }

    sort(inserts.begin(), inserts.end(), record_less);
    sort(removals.begin(), removals.end(), record_less);

    // Stream sorted file and merge with inserts, skipping removals
    ifstream sin;
    bool has_sorted = false;
    Record sorted_r;
    if (file_exists(sp)) {
        sin.open(sp, ios::binary);
        sin.read(reinterpret_cast<char*>(&sorted_r), RECORD_SIZE);
        has_sorted = sin.good();
    }

    ofstream tout(tmp_path(), ios::binary | ios::trunc);
    if (!tout) { sin.close(); return; }

    size_t ri = 0, ii = 0;

    while (has_sorted || ii < inserts.size()) {
        bool take_sorted;
        if (!has_sorted) {
            take_sorted = false;
        } else if (ii >= inserts.size()) {
            take_sorted = true;
        } else if (record_less(sorted_r, inserts[ii])) {
            take_sorted = true;
        } else if (record_less(inserts[ii], sorted_r)) {
            take_sorted = false;
        } else {
            // Equal: sorted_r == inserts[ii]
            // Check if sorted_r should be removed
            bool sorted_is_removed = false;
            while (ri < removals.size()) {
                if (record_less(removals[ri], sorted_r)) { ri++; continue; }
                if (record_less(sorted_r, removals[ri])) break;
                sorted_is_removed = true; ri++; break;
            }

            if (sorted_is_removed) {
                // Take the insert instead of the removed sorted entry
                tout.write(reinterpret_cast<const char*>(&inserts[ii]), RECORD_SIZE);
            } else {
                // Keep sorted, skip duplicate insert
                tout.write(reinterpret_cast<const char*>(&sorted_r), RECORD_SIZE);
            }

            sin.read(reinterpret_cast<char*>(&sorted_r), RECORD_SIZE);
            has_sorted = sin.good();
            ii++;
            continue;
        }

        if (take_sorted) {
            // Check if sorted_r should be removed
            bool removed = false;
            while (ri < removals.size()) {
                if (record_less(removals[ri], sorted_r)) { ri++; continue; }
                if (record_less(sorted_r, removals[ri])) break;
                removed = true; ri++; break;
            }
            if (!removed) {
                tout.write(reinterpret_cast<const char*>(&sorted_r), RECORD_SIZE);
            }
            sin.read(reinterpret_cast<char*>(&sorted_r), RECORD_SIZE);
            has_sorted = sin.good();
        } else {
            tout.write(reinterpret_cast<const char*>(&inserts[ii]), RECORD_SIZE);
            ii++;
        }
    }

    sin.close();
    tout.close();
    rename(tmp_path().c_str(), sp.c_str());

    ofstream lclear(lp, ios::binary | ios::trunc);
    lclear.close();
}

static void do_insert(const string& index, int value) {
    Record r;
    pad_index(index, r.index);
    r.value = value;
    r.flag = 0;
    ofstream out(log_path(), ios::binary | ios::app);
    out.write(reinterpret_cast<const char*>(&r), RECORD_SIZE);
}

static void do_delete(const string& index, int value) {
    Record r;
    pad_index(index, r.index);
    r.value = value;
    r.flag = 1;
    ofstream out(log_path(), ios::binary | ios::app);
    out.write(reinterpret_cast<const char*>(&r), RECORD_SIZE);
}

static void do_find(const string& index) {
    char target[INDEX_SIZE];
    pad_index(index, target);

    vector<int> values;

    string sp = sorted_path();
    FILE* f = fopen(sp.c_str(), "rb");
    if (f) {
        int count = sorted_count(f);
        int first = binary_search_sorted(f, target, count);
        if (first >= 0) {
            Record r;
            for (int i = first; i < count; i++) {
                if (!read_sorted_record(f, i, r)) break;
                if (cmp_index(r.index, target) != 0) break;
                values.push_back(r.value);
            }
        }
        fclose(f);
    }

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

        if (i % 500 == 499) merge();
    }

    merge();
    cout.flush();
    return 0;
}