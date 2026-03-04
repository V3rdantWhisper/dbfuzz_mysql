/* ═══════════════════════════════════════════════════════════════════
 * test_sqlcom_slots.cpp
 * Unit tests for load_node_sqlcom_mapping() and pick_weighted_node()
 *
 * Compile:
 *   g++ -std=c++17 -g -O0 -o test_sqlcom_slots test_sqlcom_slots.cpp
 *
 * Run:
 *   ./test_sqlcom_slots
 *
 * Exit code 0 = all tests passed.
 * ═══════════════════════════════════════════════════════════════════ */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>
#include <fstream>
#include <algorithm>
#include <functional>
#include <cmath>

using namespace std;

/* ── Minimal AFL type stubs ──────────────────────────────────────── */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;

#define MAX_SQLCOM 161

/* Controllable UR() stub: by default uses random(), but tests can
 * inject a deterministic sequence via ur_inject(). */
static vector<u32> g_ur_sequence;
static size_t      g_ur_idx = 0;

static inline u32 UR(u32 limit) {
  if (!g_ur_sequence.empty() && g_ur_idx < g_ur_sequence.size()) {
    u32 val = g_ur_sequence[g_ur_idx++] % limit;
    return val;
  }
  return (u32)(random() % limit);
}

static void ur_inject(const vector<u32> &seq) {
  g_ur_sequence = seq;
  g_ur_idx = 0;
}

static void ur_reset() {
  g_ur_sequence.clear();
  g_ur_idx = 0;
}

/* ── Globals replicated from afl-fuzz.cpp ────────────────────────── */
static u64 sqlcom_exec_count[MAX_SQLCOM];

struct SqlcomSlot {
  int            sqlcom_int;
  vector<string> roots;
};
static vector<SqlcomSlot> g_sqlcom_slots;

/* ══════════════════════════════════════════════════════════════════
 * Copy of load_node_sqlcom_mapping() and pick_weighted_node()
 * exactly as they appear in afl-fuzz.cpp.
 * ══════════════════════════════════════════════════════════════════ */
static void load_node_sqlcom_mapping() {
  static const set<string> FIXED_SLOTS = {
    "create_table_stmt", "insert_stmt", "select_stmt"
  };

  ifstream f("./node_sqlcom_mapping.json");
  if (!f.is_open()) {
    fprintf(stderr, "[SQLCOM] WARNING: ./node_sqlcom_mapping.json not found, "
                    "falling back to simple_statement only.\n");
    return;
  }

  unordered_map<int, set<string>> sc_to_roots;

  string line, cur_node;
  bool   in_sqlcom_values = false;

  while (getline(f, line)) {
    if (line.find("\"diff_mappings\"") != string::npos) break;

    size_t np = line.find("\"node\":");
    if (np != string::npos) {
      size_t q1 = line.find('"', np + 7);
      if (q1 != string::npos) {
        size_t q2 = line.find('"', q1 + 1);
        if (q2 != string::npos) {
          cur_node         = line.substr(q1 + 1, q2 - q1 - 1);
          in_sqlcom_values = false;
        }
      }
      continue;
    }

    if (cur_node.empty()) continue;

    size_t sv = line.find("\"sqlcom_values\":");
    if (sv != string::npos) {
      if (line.find("null", sv) != string::npos) {
        cur_node.clear();
        continue;
      }
      in_sqlcom_values = (line.find('[', sv) != string::npos);
    }

    if (!in_sqlcom_values) continue;
    if (line.find(']') != string::npos) in_sqlcom_values = false;

    size_t pos = 0;
    while (pos < line.size()) {
      while (pos < line.size() && !isdigit((unsigned char)line[pos])) pos++;
      if (pos >= line.size()) break;
      size_t end = pos;
      while (end < line.size() && isdigit((unsigned char)line[end])) end++;
      int val = -1;
      try { val = stoi(line.substr(pos, end - pos)); }
      catch (...) {}
      if (val >= 0 && val < MAX_SQLCOM
          && FIXED_SLOTS.find(cur_node) == FIXED_SLOTS.end()) {
        sc_to_roots[val].insert(cur_node);
      }
      pos = end;
    }
  }

  for (auto &kv : sc_to_roots) {
    SqlcomSlot slot;
    slot.sqlcom_int = kv.first;
    for (auto &r : kv.second) slot.roots.push_back(r);
    g_sqlcom_slots.push_back(slot);
  }

  fprintf(stderr,
    "[SQLCOM] Loaded %zu SQLCOM slots (covering %zu unique grammar roots).\n",
    g_sqlcom_slots.size(),
    [&]() { set<string> s; for (auto &sl : g_sqlcom_slots)
             for (auto &r : sl.roots) s.insert(r); return s.size(); }());
}

static string pick_weighted_node() {
  if (g_sqlcom_slots.empty()) return "simple_statement";

  double total_weight = 0.0;
  vector<double> weights(g_sqlcom_slots.size());
  for (size_t i = 0; i < g_sqlcom_slots.size(); i++) {
    u64 cnt = sqlcom_exec_count[g_sqlcom_slots[i].sqlcom_int];
    weights[i]    = 1.0 / (double)(cnt + 1);
    total_weight += weights[i];
  }

  double r = ((double)UR(10000) / 10000.0) * total_weight;
  double cumulative = 0.0;
  size_t chosen = g_sqlcom_slots.size() - 1;
  for (size_t i = 0; i < g_sqlcom_slots.size(); i++) {
    cumulative += weights[i];
    if (r <= cumulative) { chosen = i; break; }
  }

  const auto &roots = g_sqlcom_slots[chosen].roots;
  if (roots.size() == 1) return roots[0];
  return roots[UR((u32)roots.size())];
}

/* ══════════════════════════════════════════════════════════════════
 * Test infrastructure
 * ══════════════════════════════════════════════════════════════════ */
static int  g_tests_passed = 0;
static int  g_tests_failed = 0;
static bool g_test_ok      = true;   /* shared between TEST() and ASSERT macros */

#define TEST(name)                                                 \
  do {                                                             \
    fprintf(stderr, "  [TEST] %-55s ", #name);                     \
    g_test_ok = true;                                              \
    try { name(); }                                                \
    catch (const exception &e) {                                   \
      fprintf(stderr, "EXCEPTION: %s\n", e.what());               \
      g_test_ok = false;                                           \
    }                                                              \
    if (g_test_ok) { fprintf(stderr, "PASS\n"); g_tests_passed++; } \
    else           { g_tests_failed++; }                           \
  } while (0)

/* fmt_val: format values for diagnostic output in ASSERT macros.
 * Separate name avoids ambiguity with std::to_string. */
static string fmt_val(const string &s) { return "\"" + s + "\""; }
static string fmt_val(size_t v) { return std::to_string(v); }
static string fmt_val(int v) { return std::to_string(v); }
static string fmt_val(bool v) { return v ? "true" : "false"; }

#define ASSERT_EQ(a, b)                                            \
  do {                                                             \
    auto _a = (a); auto _b = (b);                                  \
    if (_a != _b) {                                                \
      fprintf(stderr, "FAIL\n    ASSERT_EQ failed at %s:%d\n"     \
              "      got:      %s\n      expected: %s\n",          \
              __FILE__, __LINE__,                                   \
              fmt_val(_a).c_str(), fmt_val(_b).c_str());           \
      g_test_ok = false; return;                                   \
    }                                                              \
  } while (0)

#define ASSERT_TRUE(expr)                                          \
  do {                                                             \
    if (!(expr)) {                                                 \
      fprintf(stderr, "FAIL\n    ASSERT_TRUE failed at %s:%d\n"   \
              "      expression: %s\n", __FILE__, __LINE__, #expr);\
      g_test_ok = false; return;                                   \
    }                                                              \
  } while (0)

/* Helper: write a temporary JSON file and load it. */
static void setup_json(const string &content) {
  g_sqlcom_slots.clear();
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));
  ur_reset();
  {
    ofstream out("./node_sqlcom_mapping.json");
    out << content;
  }
  load_node_sqlcom_mapping();
}

static void cleanup_json() {
  remove("./node_sqlcom_mapping.json");
  g_sqlcom_slots.clear();
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));
  ur_reset();
}

/* ── Helper: find a slot by sqlcom_int, return nullptr if not found ── */
static const SqlcomSlot *find_slot(int sc) {
  for (auto &s : g_sqlcom_slots)
    if (s.sqlcom_int == sc) return &s;
  return nullptr;
}

/* ══════════════════════════════════════════════════════════════════
 * TEST CASES — load_node_sqlcom_mapping()
 * ══════════════════════════════════════════════════════════════════ */

/* T1: file not found → g_sqlcom_slots empty */
void test_load_missing_file() {
  g_sqlcom_slots.clear();
  remove("./node_sqlcom_mapping.json");
  load_node_sqlcom_mapping();
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)0);
}

/* T2: single-SQLCOM node → exactly one slot with one root */
void test_load_single_sqlcom_node() {
  setup_json(R"({
    "mappings": [
      {
        "node": "alter_event_stmt",
        "sqlcom_values": [123],
        "sqlcom_names": ["SQLCOM_ALTER_EVENT"],
        "dominant_sqlcom": 123
      }
    ],
    "diff_mappings": []
  })");
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)1);
  ASSERT_EQ(g_sqlcom_slots[0].sqlcom_int, 123);
  ASSERT_EQ(g_sqlcom_slots[0].roots.size(), (size_t)1);
  ASSERT_EQ(g_sqlcom_slots[0].roots[0], string("alter_event_stmt"));
  cleanup_json();
}

/* T3: multi-SQLCOM node → N separate slots, each with the same root */
void test_load_multi_sqlcom_node() {
  setup_json(R"({
    "mappings": [
      {
        "node": "create",
        "sqlcom_values": [36, 42, 84],
        "sqlcom_names": ["SQLCOM_CREATE_DB","SQLCOM_CREATE_FUNCTION","SQLCOM_CREATE_USER"],
        "dominant_sqlcom": 42
      }
    ],
    "diff_mappings": []
  })");
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)3);
  /* Each SQLCOM should have its own slot, all with root "create" */
  for (auto &sl : g_sqlcom_slots) {
    ASSERT_EQ(sl.roots.size(), (size_t)1);
    ASSERT_EQ(sl.roots[0], string("create"));
    ASSERT_TRUE(sl.sqlcom_int == 36 || sl.sqlcom_int == 42 || sl.sqlcom_int == 84);
  }
  /* Verify no duplicate SQLCOM */
  set<int> seen;
  for (auto &sl : g_sqlcom_slots) {
    ASSERT_TRUE(seen.find(sl.sqlcom_int) == seen.end());
    seen.insert(sl.sqlcom_int);
  }
  cleanup_json();
}

/* T4: same SQLCOM from two different nodes → one slot with two roots */
void test_load_shared_sqlcom() {
  setup_json(R"({
    "mappings": [
      {
        "node": "describe_stmt",
        "sqlcom_values": [13],
        "dominant_sqlcom": 13
      },
      {
        "node": "show_columns_stmt",
        "sqlcom_values": [13],
        "dominant_sqlcom": 13
      }
    ],
    "diff_mappings": []
  })");
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)1);
  ASSERT_EQ(g_sqlcom_slots[0].sqlcom_int, 13);
  ASSERT_EQ(g_sqlcom_slots[0].roots.size(), (size_t)2);
  /* roots should contain both (order is set-sorted) */
  set<string> roots_set(g_sqlcom_slots[0].roots.begin(),
                        g_sqlcom_slots[0].roots.end());
  ASSERT_TRUE(roots_set.count("describe_stmt") == 1);
  ASSERT_TRUE(roots_set.count("show_columns_stmt") == 1);
  cleanup_json();
}

/* T5: fixed-slot nodes (create_table_stmt, insert_stmt, select_stmt)
 *     must be excluded even if they have valid sqlcom_values */
void test_load_excludes_fixed_slots() {
  setup_json(R"({
    "mappings": [
      {
        "node": "create_table_stmt",
        "sqlcom_values": [1],
        "dominant_sqlcom": 1
      },
      {
        "node": "insert_stmt",
        "sqlcom_values": [5, 6],
        "dominant_sqlcom": 5
      },
      {
        "node": "select_stmt",
        "sqlcom_values": [0],
        "dominant_sqlcom": 0
      },
      {
        "node": "drop_table_stmt",
        "sqlcom_values": [9],
        "dominant_sqlcom": 9
      }
    ],
    "diff_mappings": []
  })");
  /* Only drop_table_stmt should survive */
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)1);
  ASSERT_EQ(g_sqlcom_slots[0].sqlcom_int, 9);
  ASSERT_EQ(g_sqlcom_slots[0].roots[0], string("drop_table_stmt"));
  cleanup_json();
}

/* T6: sqlcom_values: null → node is skipped */
void test_load_null_sqlcom_values() {
  setup_json(R"({
    "mappings": [
      {
        "node": "alter_database_stmt",
        "sqlcom_values": null,
        "dominant_sqlcom": -1
      },
      {
        "node": "truncate_stmt",
        "sqlcom_values": [8],
        "dominant_sqlcom": 8
      }
    ],
    "diff_mappings": []
  })");
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)1);
  ASSERT_EQ(g_sqlcom_slots[0].sqlcom_int, 8);
  cleanup_json();
}

/* T7: diff_mappings section is completely ignored (not loaded) */
void test_load_ignores_diff_mappings() {
  setup_json(R"({
    "mappings": [
      {
        "node": "purge",
        "sqlcom_values": [66, 67],
        "dominant_sqlcom": 66
      }
    ],
    "diff_mappings": [
      {
        "node": "purge_option",
        "parent_root": "purge",
        "only_in_with": ["SQLCOM_PURGE_BEFORE"]
      }
    ]
  })");
  /* Only the 2 SQLCOMs from mappings should be loaded */
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)2);
  /* diff_mappings should NOT introduce any additional slots */
  set<int> sc_set;
  for (auto &sl : g_sqlcom_slots) sc_set.insert(sl.sqlcom_int);
  ASSERT_TRUE(sc_set.count(66) == 1);
  ASSERT_TRUE(sc_set.count(67) == 1);
  cleanup_json();
}

/* T8: values at boundary (0 and MAX_SQLCOM-1=160) are accepted,
 *     values >= MAX_SQLCOM are rejected */
void test_load_boundary_sqlcom_values() {
  setup_json(R"({
    "mappings": [
      {
        "node": "node_zero",
        "sqlcom_values": [0],
        "dominant_sqlcom": 0
      },
      {
        "node": "node_max_valid",
        "sqlcom_values": [160],
        "dominant_sqlcom": 160
      },
      {
        "node": "node_overflow",
        "sqlcom_values": [161],
        "dominant_sqlcom": 161
      },
      {
        "node": "node_big",
        "sqlcom_values": [9999],
        "dominant_sqlcom": 9999
      }
    ],
    "diff_mappings": []
  })");
  /* 0 and 160 accepted; 161 and 9999 rejected */
  set<int> sc_set;
  for (auto &sl : g_sqlcom_slots) sc_set.insert(sl.sqlcom_int);
  ASSERT_TRUE(sc_set.count(0) == 1);
  ASSERT_TRUE(sc_set.count(160) == 1);
  ASSERT_TRUE(sc_set.count(161) == 0);
  ASSERT_TRUE(sc_set.count(9999) == 0);
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)2);
  cleanup_json();
}

/* T9: one slot per unique SQLCOM — no duplicate slots */
void test_load_no_duplicate_slots() {
  setup_json(R"({
    "mappings": [
      {
        "node": "node_a",
        "sqlcom_values": [10, 20],
        "dominant_sqlcom": 10
      },
      {
        "node": "node_b",
        "sqlcom_values": [20, 30],
        "dominant_sqlcom": 20
      }
    ],
    "diff_mappings": []
  })");
  /* Expect 3 slots: 10, 20, 30.  SQLCOM 20 from both nodes → merged. */
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)3);
  set<int> sc_set;
  for (auto &sl : g_sqlcom_slots) {
    ASSERT_TRUE(sc_set.find(sl.sqlcom_int) == sc_set.end());
    sc_set.insert(sl.sqlcom_int);
  }
  ASSERT_TRUE(sc_set.count(10) && sc_set.count(20) && sc_set.count(30));
  /* SQLCOM 20 should have two roots */
  auto *sl20 = find_slot(20);
  ASSERT_TRUE(sl20 != nullptr);
  ASSERT_EQ(sl20->roots.size(), (size_t)2);
  cleanup_json();
}

/* T10: sqlcom_values on multiple lines */
void test_load_multiline_sqlcom_values() {
  setup_json(
    "{\n"
    "  \"mappings\": [\n"
    "    {\n"
    "      \"node\": \"xa\",\n"
    "      \"sqlcom_values\": [\n"
    "        106,\n"
    "        107,\n"
    "        108\n"
    "      ],\n"
    "      \"dominant_sqlcom\": 106\n"
    "    }\n"
    "  ],\n"
    "  \"diff_mappings\": []\n"
    "}\n"
  );
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)3);
  set<int> sc_set;
  for (auto &sl : g_sqlcom_slots) sc_set.insert(sl.sqlcom_int);
  ASSERT_TRUE(sc_set.count(106) && sc_set.count(107) && sc_set.count(108));
  for (auto &sl : g_sqlcom_slots) {
    ASSERT_EQ(sl.roots.size(), (size_t)1);
    ASSERT_EQ(sl.roots[0], string("xa"));
  }
  cleanup_json();
}

/* T11: load against the REAL node_sqlcom_mapping.json from fuzz_root */
void test_load_real_json() {
  /* Copy the real JSON into CWD */
  g_sqlcom_slots.clear();
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));
  ur_reset();

  /* Try to symlink or copy from known locations */
  const char *real_paths[] = {
    "../../fuzz_root/node_sqlcom_mapping.json",
    "../../../fuzz_root/node_sqlcom_mapping.json",
    nullptr
  };
  bool found = false;
  for (int i = 0; real_paths[i]; i++) {
    ifstream check(real_paths[i]);
    if (check.is_open()) {
      check.close();
      /* Copy file */
      ifstream src(real_paths[i], ios::binary);
      ofstream dst("./node_sqlcom_mapping.json", ios::binary);
      dst << src.rdbuf();
      found = true;
      break;
    }
  }
  if (!found) {
    fprintf(stderr, "SKIP (real JSON not found)\n");
    g_tests_passed++; /* count as pass — just not available */
    return;
  }

  load_node_sqlcom_mapping();

  /* ── Verify against known properties ── */
  /* 138 unique SQLCOMs from Python analysis */
  ASSERT_EQ(g_sqlcom_slots.size(), (size_t)138);

  /* No SQLCOM_END (159) should be present */
  ASSERT_TRUE(find_slot(159) == nullptr);

  /* All sqlcom_int values in range [0, MAX_SQLCOM) */
  for (auto &sl : g_sqlcom_slots) {
    ASSERT_TRUE(sl.sqlcom_int >= 0);
    ASSERT_TRUE(sl.sqlcom_int < MAX_SQLCOM);
    ASSERT_TRUE(!sl.roots.empty());
  }

  /* No duplicate SQLCOM slots */
  set<int> sc_set;
  for (auto &sl : g_sqlcom_slots) {
    ASSERT_TRUE(sc_set.find(sl.sqlcom_int) == sc_set.end());
    sc_set.insert(sl.sqlcom_int);
  }

  /* Fixed slots should not appear as roots */
  for (auto &sl : g_sqlcom_slots) {
    for (auto &r : sl.roots) {
      ASSERT_TRUE(r != "create_table_stmt");
      ASSERT_TRUE(r != "insert_stmt");
      ASSERT_TRUE(r != "select_stmt");
    }
  }

  /* Known multi-root SQLCOM: SQLCOM_SHOW_FIELDS=13 has 2 roots */
  auto *sl13 = find_slot(13);
  ASSERT_TRUE(sl13 != nullptr);
  ASSERT_EQ(sl13->roots.size(), (size_t)2);
  set<string> r13(sl13->roots.begin(), sl13->roots.end());
  ASSERT_TRUE(r13.count("describe_stmt") == 1);
  ASSERT_TRUE(r13.count("show_columns_stmt") == 1);

  /* Known multi-root SQLCOM: SQLCOM_ALTER_TABLESPACE=114 has 7 roots */
  auto *sl114 = find_slot(114);
  ASSERT_TRUE(sl114 != nullptr);
  ASSERT_EQ(sl114->roots.size(), (size_t)7);

  /* Known: "create" node should appear in SQLCOM 36,42,84,102,104,114,119 */
  set<int> create_sqlcoms = {36, 42, 84, 102, 104, 114, 119};
  for (int sc : create_sqlcoms) {
    auto *sl = find_slot(sc);
    ASSERT_TRUE(sl != nullptr);
    bool has_create = false;
    for (auto &r : sl->roots) if (r == "create") has_create = true;
    ASSERT_TRUE(has_create);
  }

  /* Count unique roots — should be 120 from analysis */
  set<string> all_roots;
  for (auto &sl : g_sqlcom_slots)
    for (auto &r : sl.roots) all_roots.insert(r);
  ASSERT_EQ(all_roots.size(), (size_t)120);

  cleanup_json();
}

/* ══════════════════════════════════════════════════════════════════
 * TEST CASES — pick_weighted_node()
 * ══════════════════════════════════════════════════════════════════ */

/* T12: empty slots → "simple_statement" */
void test_pick_empty_slots() {
  g_sqlcom_slots.clear();
  string result = pick_weighted_node();
  ASSERT_EQ(result, string("simple_statement"));
}

/* T13: single slot, single root → always returns that root */
void test_pick_single_slot_single_root() {
  g_sqlcom_slots.clear();
  g_sqlcom_slots.push_back({42, {"create"}});
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));

  /* Try multiple times — should always return "create" */
  for (int i = 0; i < 100; i++) {
    string result = pick_weighted_node();
    ASSERT_EQ(result, string("create"));
  }
}

/* T14: single slot, multiple roots → UR selects among roots */
void test_pick_single_slot_multi_root() {
  g_sqlcom_slots.clear();
  g_sqlcom_slots.push_back({13, {"describe_stmt", "show_columns_stmt"}});
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));

  /* Inject UR: first call for slot selection, second for root selection.
   * UR(10000) → 0 means r=0.0 → picks slot 0 (cumulative >= 0 immediately).
   * UR(2) → 0 means pick roots[0] = "describe_stmt". */
  ur_inject({0, 0});
  ASSERT_EQ(pick_weighted_node(), string("describe_stmt"));

  /* UR(10000) → 0, UR(2) → 1 → roots[1] = "show_columns_stmt" */
  ur_inject({0, 1});
  ASSERT_EQ(pick_weighted_node(), string("show_columns_stmt"));
}

/* T15: inverse-frequency weighting — heavily exercised SQLCOM gets less weight.
 *
 * Setup: 2 slots.
 *   Slot 0: SQLCOM=10, exec_count=0  → weight = 1/(0+1) = 1.0
 *   Slot 1: SQLCOM=20, exec_count=99 → weight = 1/(99+1) = 0.01
 *   total_weight = 1.01
 *
 *   UR(10000) → 0  ⇒ r=0.0      → slot 0 (cumulative 1.0 >= 0.0)
 *   UR(10000) → 9999 ⇒ r≈1.0099 → slot 1 (cumulative 1.01 >= 1.0099)
 */
void test_pick_weighted_favors_low_count() {
  g_sqlcom_slots.clear();
  g_sqlcom_slots.push_back({10, {"node_a"}});
  g_sqlcom_slots.push_back({20, {"node_b"}});
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));
  sqlcom_exec_count[10] = 0;
  sqlcom_exec_count[20] = 99;

  /* UR=0 → picks slot 0 (lowest cumulative threshold passes first) */
  ur_inject({0});
  ASSERT_EQ(pick_weighted_node(), string("node_a"));

  /* UR=9999 → r ≈ total_weight → picks last slot */
  ur_inject({9999});
  ASSERT_EQ(pick_weighted_node(), string("node_b"));
}

/* T16: when all exec_counts are equal, weights are equal → uniform selection.
 *      Statistical test: over many draws, each slot should be picked ~equally. */
void test_pick_uniform_when_equal_counts() {
  g_sqlcom_slots.clear();
  for (int i = 0; i < 5; i++)
    g_sqlcom_slots.push_back({i, {"root_" + std::to_string(i)}});
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));
  /* All counts = 0, so weights are all 1.0 */

  ur_reset(); /* use real random */
  srandom(12345);

  unordered_map<string, int> freq;
  int N = 50000;
  for (int i = 0; i < N; i++) {
    string r = pick_weighted_node();
    freq[r]++;
  }

  /* Each root should get roughly N/5 = 10000 picks.
   * With 50k trials and 5 bins, allow ±20% tolerance. */
  double expected = (double)N / 5.0;
  for (int i = 0; i < 5; i++) {
    string key = "root_" + std::to_string(i);
    double actual = (double)freq[key];
    double ratio = actual / expected;
    ASSERT_TRUE(ratio > 0.8 && ratio < 1.2);
  }
}

/* T17: pick heavily favors the under-exercised SQLCOM.
 *      Setup: 2 slots, one has count=0, other has count=10000.
 *      The ratio of weights is 1.0 : 0.0001, so >99.99% picks go to slot 0. */
void test_pick_statistical_bias() {
  g_sqlcom_slots.clear();
  g_sqlcom_slots.push_back({10, {"rare_node"}});
  g_sqlcom_slots.push_back({20, {"common_node"}});
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));
  sqlcom_exec_count[10] = 0;       /* weight = 1.0   */
  sqlcom_exec_count[20] = 10000;   /* weight ≈ 0.0001 */

  ur_reset();
  srandom(54321);

  int rare_count = 0;
  int N = 10000;
  for (int i = 0; i < N; i++) {
    if (pick_weighted_node() == "rare_node") rare_count++;
  }

  /* rare_node should get >99% of picks */
  double ratio = (double)rare_count / (double)N;
  ASSERT_TRUE(ratio > 0.99);
}

/* T18: pick adapts as counts change — initially uniform,
 *      then biased after one SQLCOM gets exercised. */
void test_pick_adapts_to_count_changes() {
  g_sqlcom_slots.clear();
  g_sqlcom_slots.push_back({10, {"node_a"}});
  g_sqlcom_slots.push_back({20, {"node_b"}});
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));

  ur_reset();
  srandom(99999);

  /* Phase 1: both counts=0 → roughly uniform */
  unordered_map<string, int> freq1;
  for (int i = 0; i < 10000; i++) freq1[pick_weighted_node()]++;
  double r1 = (double)freq1["node_a"] / 10000.0;
  ASSERT_TRUE(r1 > 0.4 && r1 < 0.6); /* ~50% */

  /* Phase 2: heavily exercise SQLCOM 10 */
  sqlcom_exec_count[10] = 100000;

  unordered_map<string, int> freq2;
  for (int i = 0; i < 10000; i++) freq2[pick_weighted_node()]++;
  double r2 = (double)freq2["node_b"] / 10000.0;
  ASSERT_TRUE(r2 > 0.99); /* node_b should dominate */
}

/* T19: UR is actually called (not bypassed) — verify by injecting a
 *      known sequence and checking deterministic behavior. */
void test_pick_uses_ur() {
  g_sqlcom_slots.clear();
  g_sqlcom_slots.push_back({10, {"alpha"}});
  g_sqlcom_slots.push_back({20, {"beta"}});
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));

  /* With equal weights: total_weight=2.0, each slot weight=1.0.
   * UR(10000)=0 → r=0.0 → cumulative 1.0 >= 0.0 → slot 0 → "alpha"
   * UR(10000)=5001 → r=5001/10000*2.0=1.0002 → cumulative 1.0 < 1.0002,
   *   cumulative 2.0 >= 1.0002 → slot 1 → "beta" */
  ur_inject({0});
  ASSERT_EQ(pick_weighted_node(), string("alpha"));

  ur_inject({5001});
  ASSERT_EQ(pick_weighted_node(), string("beta"));
}

/* T20: stress test — no crash/UB with many slots */
void test_pick_stress_many_slots() {
  g_sqlcom_slots.clear();
  for (int i = 0; i < MAX_SQLCOM - 1; i++) { /* 160 slots */
    g_sqlcom_slots.push_back({i, {"root_" + std::to_string(i)}});
  }
  memset(sqlcom_exec_count, 0, sizeof(sqlcom_exec_count));

  ur_reset();
  srandom(77777);

  /* Just verify no crash in 1000 picks */
  for (int i = 0; i < 1000; i++) {
    string r = pick_weighted_node();
    ASSERT_TRUE(!r.empty());
    ASSERT_TRUE(r.find("root_") == 0);
  }
}

/* ══════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════ */
int main() {
  fprintf(stderr, "\n══════════════════════════════════════════════════════\n");
  fprintf(stderr, " Unit Tests: load_node_sqlcom_mapping / pick_weighted_node\n");
  fprintf(stderr, "══════════════════════════════════════════════════════\n\n");

  fprintf(stderr, "── load_node_sqlcom_mapping() ─────────────────────\n");
  TEST(test_load_missing_file);
  TEST(test_load_single_sqlcom_node);
  TEST(test_load_multi_sqlcom_node);
  TEST(test_load_shared_sqlcom);
  TEST(test_load_excludes_fixed_slots);
  TEST(test_load_null_sqlcom_values);
  TEST(test_load_ignores_diff_mappings);
  TEST(test_load_boundary_sqlcom_values);
  TEST(test_load_no_duplicate_slots);
  TEST(test_load_multiline_sqlcom_values);
  TEST(test_load_real_json);

  fprintf(stderr, "\n── pick_weighted_node() ───────────────────────────\n");
  TEST(test_pick_empty_slots);
  TEST(test_pick_single_slot_single_root);
  TEST(test_pick_single_slot_multi_root);
  TEST(test_pick_weighted_favors_low_count);
  TEST(test_pick_uniform_when_equal_counts);
  TEST(test_pick_statistical_bias);
  TEST(test_pick_adapts_to_count_changes);
  TEST(test_pick_uses_ur);
  TEST(test_pick_stress_many_slots);

  fprintf(stderr, "\n══════════════════════════════════════════════════════\n");
  fprintf(stderr, " Results: %d passed, %d failed\n",
          g_tests_passed, g_tests_failed);
  fprintf(stderr, "══════════════════════════════════════════════════════\n\n");

  cleanup_json();
  return g_tests_failed > 0 ? 1 : 0;
}
