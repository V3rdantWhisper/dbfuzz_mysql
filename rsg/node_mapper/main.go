// node_mapper — discovers mappings from MySQL grammar AST nodes to sql_command values.
//
// Usage:
//
//	./node_mapper [flags]
//
// Flags:
//
//	-grammar   path to the MySQL .y grammar file   (default: ../parser_def_files/mysql_sql.y)
//	-executor  path to the compiled C++ executor   (default: ./executor/executor)
//	-log-file  path to mysqld's stderr log file    (default: /tmp/mysql_stderr.log)
//	-output    output JSON file path               (default: ./node_sqlcom_mapping.json)
//	-n         SQL queries to generate per node    (default: 200)
//	-depth     RSG generation depth                (default: 2)
//	-seed      random seed (0 = time-based)        (default: 0)
//	-extra-roots comma-separated extra root nodes to probe beyond simple_statement children
//
// Architecture:
//  1. Parse the MySQL grammar and build a SimpleRSG.
//  2. Enumerate every direct child of simple_statement as a candidate "root".
//  3. For each root, generate -n SQL strings and stream them to the C++ executor
//     via stdin/stdout pipes.  The executor executes each SQL against a live
//     MySQL 8.0.33 instance, reads the SQLCOM_EXEC line from mysqld's stderr log,
//     and echoes back: "<node>\t<sqlcom_int>\t<status>\n".
//  4. Aggregate results and emit a JSON mapping file.
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"math/rand"
	"os"
	"os/exec"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/rsg/yacc"
)

// ---------------------------------------------------------------------------
// enum_sql_command name table (MySQL 8.0.33 — my_sqlcommand.h)
// ---------------------------------------------------------------------------

var sqlcomNames = []string{
	"SQLCOM_SELECT",
	"SQLCOM_CREATE_TABLE",
	"SQLCOM_CREATE_INDEX",
	"SQLCOM_ALTER_TABLE",
	"SQLCOM_UPDATE",
	"SQLCOM_INSERT",
	"SQLCOM_INSERT_SELECT",
	"SQLCOM_DELETE",
	"SQLCOM_TRUNCATE",
	"SQLCOM_DROP_TABLE",
	"SQLCOM_DROP_INDEX",
	"SQLCOM_SHOW_DATABASES",
	"SQLCOM_SHOW_TABLES",
	"SQLCOM_SHOW_FIELDS",
	"SQLCOM_SHOW_KEYS",
	"SQLCOM_SHOW_VARIABLES",
	"SQLCOM_SHOW_STATUS",
	"SQLCOM_SHOW_ENGINE_LOGS",
	"SQLCOM_SHOW_ENGINE_STATUS",
	"SQLCOM_SHOW_ENGINE_MUTEX",
	"SQLCOM_SHOW_PROCESSLIST",
	"SQLCOM_SHOW_MASTER_STAT",
	"SQLCOM_SHOW_SLAVE_STAT",
	"SQLCOM_SHOW_GRANTS",
	"SQLCOM_SHOW_CREATE",
	"SQLCOM_SHOW_CHARSETS",
	"SQLCOM_SHOW_COLLATIONS",
	"SQLCOM_SHOW_CREATE_DB",
	"SQLCOM_SHOW_TABLE_STATUS",
	"SQLCOM_SHOW_TRIGGERS",
	"SQLCOM_LOAD",
	"SQLCOM_SET_OPTION",
	"SQLCOM_LOCK_TABLES",
	"SQLCOM_UNLOCK_TABLES",
	"SQLCOM_GRANT",
	"SQLCOM_CHANGE_DB",
	"SQLCOM_CREATE_DB",
	"SQLCOM_DROP_DB",
	"SQLCOM_ALTER_DB",
	"SQLCOM_REPAIR",
	"SQLCOM_REPLACE",
	"SQLCOM_REPLACE_SELECT",
	"SQLCOM_CREATE_FUNCTION",
	"SQLCOM_DROP_FUNCTION",
	"SQLCOM_REVOKE",
	"SQLCOM_OPTIMIZE",
	"SQLCOM_CHECK",
	"SQLCOM_ASSIGN_TO_KEYCACHE",
	"SQLCOM_PRELOAD_KEYS",
	"SQLCOM_FLUSH",
	"SQLCOM_KILL",
	"SQLCOM_ANALYZE",
	"SQLCOM_ROLLBACK",
	"SQLCOM_ROLLBACK_TO_SAVEPOINT",
	"SQLCOM_COMMIT",
	"SQLCOM_SAVEPOINT",
	"SQLCOM_RELEASE_SAVEPOINT",
	"SQLCOM_SLAVE_START",
	"SQLCOM_SLAVE_STOP",
	"SQLCOM_START_GROUP_REPLICATION",
	"SQLCOM_STOP_GROUP_REPLICATION",
	"SQLCOM_BEGIN",
	"SQLCOM_CHANGE_MASTER",
	"SQLCOM_CHANGE_REPLICATION_FILTER",
	"SQLCOM_RENAME_TABLE",
	"SQLCOM_RESET",
	"SQLCOM_PURGE",
	"SQLCOM_PURGE_BEFORE",
	"SQLCOM_SHOW_BINLOGS",
	"SQLCOM_SHOW_OPEN_TABLES",
	"SQLCOM_HA_OPEN",
	"SQLCOM_HA_CLOSE",
	"SQLCOM_HA_READ",
	"SQLCOM_SHOW_SLAVE_HOSTS",
	"SQLCOM_DELETE_MULTI",
	"SQLCOM_UPDATE_MULTI",
	"SQLCOM_SHOW_BINLOG_EVENTS",
	"SQLCOM_DO",
	"SQLCOM_SHOW_WARNS",
	"SQLCOM_EMPTY_QUERY",
	"SQLCOM_SHOW_ERRORS",
	"SQLCOM_SHOW_STORAGE_ENGINES",
	"SQLCOM_SHOW_PRIVILEGES",
	"SQLCOM_HELP",
	"SQLCOM_CREATE_USER",
	"SQLCOM_DROP_USER",
	"SQLCOM_RENAME_USER",
	"SQLCOM_REVOKE_ALL",
	"SQLCOM_CHECKSUM",
	"SQLCOM_CREATE_PROCEDURE",
	"SQLCOM_CREATE_SPFUNCTION",
	"SQLCOM_CALL",
	"SQLCOM_DROP_PROCEDURE",
	"SQLCOM_ALTER_PROCEDURE",
	"SQLCOM_ALTER_FUNCTION",
	"SQLCOM_SHOW_CREATE_PROC",
	"SQLCOM_SHOW_CREATE_FUNC",
	"SQLCOM_SHOW_STATUS_PROC",
	"SQLCOM_SHOW_STATUS_FUNC",
	"SQLCOM_PREPARE",
	"SQLCOM_EXECUTE",
	"SQLCOM_DEALLOCATE_PREPARE",
	"SQLCOM_CREATE_VIEW",
	"SQLCOM_DROP_VIEW",
	"SQLCOM_CREATE_TRIGGER",
	"SQLCOM_DROP_TRIGGER",
	"SQLCOM_XA_START",
	"SQLCOM_XA_END",
	"SQLCOM_XA_PREPARE",
	"SQLCOM_XA_COMMIT",
	"SQLCOM_XA_ROLLBACK",
	"SQLCOM_XA_RECOVER",
	"SQLCOM_SHOW_PROC_CODE",
	"SQLCOM_SHOW_FUNC_CODE",
	"SQLCOM_ALTER_TABLESPACE",
	"SQLCOM_INSTALL_PLUGIN",
	"SQLCOM_UNINSTALL_PLUGIN",
	"SQLCOM_BINLOG_BASE64_EVENT",
	"SQLCOM_SHOW_PLUGINS",
	"SQLCOM_CREATE_SERVER",
	"SQLCOM_DROP_SERVER",
	"SQLCOM_ALTER_SERVER",
	"SQLCOM_CREATE_EVENT",
	"SQLCOM_ALTER_EVENT",
	"SQLCOM_DROP_EVENT",
	"SQLCOM_SHOW_CREATE_EVENT",
	"SQLCOM_SHOW_EVENTS",
	"SQLCOM_SHOW_CREATE_TRIGGER",
	"SQLCOM_SHOW_PROFILE",
	"SQLCOM_SHOW_PROFILES",
	"SQLCOM_SIGNAL",
	"SQLCOM_RESIGNAL",
	"SQLCOM_SHOW_RELAYLOG_EVENTS",
	"SQLCOM_GET_DIAGNOSTICS",
	"SQLCOM_ALTER_USER",
	"SQLCOM_EXPLAIN_OTHER",
	"SQLCOM_SHOW_CREATE_USER",
	"SQLCOM_SHUTDOWN",
	"SQLCOM_SET_PASSWORD",
	"SQLCOM_ALTER_INSTANCE",
	"SQLCOM_INSTALL_COMPONENT",
	"SQLCOM_UNINSTALL_COMPONENT",
	"SQLCOM_CREATE_ROLE",
	"SQLCOM_DROP_ROLE",
	"SQLCOM_SET_ROLE",
	"SQLCOM_GRANT_ROLE",
	"SQLCOM_REVOKE_ROLE",
	"SQLCOM_ALTER_USER_DEFAULT_ROLE",
	"SQLCOM_IMPORT",
	"SQLCOM_CREATE_RESOURCE_GROUP",
	"SQLCOM_ALTER_RESOURCE_GROUP",
	"SQLCOM_DROP_RESOURCE_GROUP",
	"SQLCOM_SET_RESOURCE_GROUP",
	"SQLCOM_CLONE",
	"SQLCOM_LOCK_INSTANCE",
	"SQLCOM_UNLOCK_INSTANCE",
	"SQLCOM_RESTART_SERVER",
	"SQLCOM_CREATE_SRS",
	"SQLCOM_DROP_SRS",
	"SQLCOM_END",
}

func sqlcomName(v int) string {
	if v >= 0 && v < len(sqlcomNames) {
		return sqlcomNames[v]
	}
	return fmt.Sprintf("UNKNOWN_%d", v)
}

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

// QueryRecord holds one (node, sql) pair to be executed.
type QueryRecord struct {
	Node string
	SQL  string
}

// ExecResult holds one execution result returned by the C++ executor.
type ExecResult struct {
	Node   string
	Sqlcom int    // -1 if not captured
	Status string // normal / syntax_error / semantic_error / crash / no_sqlcom
}

// NodeStats accumulates statistics for one grammar node.
type NodeStats struct {
	SqlcomFreq   map[int]int    `json:"sqlcom_freq"` // sqlcom_value → count
	StatusFreq   map[string]int `json:"status_freq"` // status → count
	TotalQueries int            `json:"total_queries"`
	ValidQueries int            `json:"valid_queries"` // status == normal
}

// MappingEntry is one node's entry in the final JSON.
type MappingEntry struct {
	Node               string    `json:"node"`
	SqlcomValues       []int     `json:"sqlcom_values"`   // sorted unique sqlcom values seen
	SqlcomNames        []string  `json:"sqlcom_names"`    // human-readable names
	DominantSqlcom     int       `json:"dominant_sqlcom"` // most frequent; -1 if none
	DominantSqlcomName string    `json:"dominant_sqlcom_name"`
	Stats              NodeStats `json:"stats"`
}

// OutputJSON is the top-level structure written to the output file.
type OutputJSON struct {
	GeneratedAt    string              `json:"generated_at"`
	GrammarFile    string              `json:"grammar_file"`
	QueriesPerNode int                 `json:"queries_per_node"`
	Mappings       []MappingEntry      `json:"mappings"`        // ordered by node name
	SqlcomToNodes  map[string][]string `json:"sqlcom_to_nodes"` // sqlcom_name → []node
}

// ---------------------------------------------------------------------------
// Grammar helpers
// ---------------------------------------------------------------------------

// directChildren returns the names of the single-token alternatives of a
// grammar rule (e.g. all child rules of simple_statement).
func directChildren(allProds map[string][]*yacc.ExpressionNode, ruleName string) []string {
	prods, ok := allProds[ruleName]
	if !ok {
		return nil
	}
	seen := make(map[string]bool)
	var result []string
	for _, prod := range prods {
		if len(prod.Items) == 1 && prod.Items[0].Typ == yacc.TypToken {
			child := prod.Items[0].Value
			if !seen[child] {
				seen[child] = true
				result = append(result, child)
			}
		}
	}
	sort.Strings(result)
	return result
}

// ---------------------------------------------------------------------------
// SQL generation
// ---------------------------------------------------------------------------

// generateQueries generates up to n SQL strings for each root in roots.
// Each item in the returned slice is a QueryRecord.
// Roots that are blacklisted inside GenerateMySQL (returning []) are skipped.
func generateQueries(rsg *SimpleRSG, roots []string, n, depth int) []QueryRecord {
	var records []QueryRecord
	for _, root := range roots {
		generated := 0
		attempts := 0
		maxAttempts := n * 20 // cap infinite loops if a node always fails
		for generated < n && attempts < maxAttempts {
			attempts++
			sql := rsg.GenerateSQL(root, depth)
			if sql == "" {
				continue
			}
			records = append(records, QueryRecord{Node: root, SQL: sql})
			generated++
		}
		if generated == 0 {
			log.Printf("[warn] node %q: could not generate any SQL in %d attempts (blacklisted or invalid root?)", root, maxAttempts)
		} else {
			log.Printf("[info] node %q: generated %d queries (%d attempts)", root, generated, attempts)
		}
	}
	return records
}

// ---------------------------------------------------------------------------
// Executor I/O
// ---------------------------------------------------------------------------

// writeQueriesFile writes records to path in TSV format: node\tSQL\n
func writeQueriesFile(path string, records []QueryRecord) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	w := bufio.NewWriter(f)
	for _, r := range records {
		// Sanitise SQL: replace tabs and newlines to keep TSV valid.
		sql := strings.ReplaceAll(r.SQL, "\t", " ")
		sql = strings.ReplaceAll(sql, "\n", " ")
		sql = strings.ReplaceAll(sql, "\r", " ")
		fmt.Fprintf(w, "%s\t%s\n", r.Node, sql)
	}
	return w.Flush()
}

// runExecutor launches the C++ executor binary, pipes queries.tsv to it and
// reads results line-by-line.  The executor protocol:
//
//	stdin  ← "node\tSQL\n"  (one query per line)
//	stdout → "node\tsqlcom_int\tstatus\n"
//
// The executor reads until EOF on stdin then exits.
func runExecutor(executorBin, queriesFile, logFile, host string, port int, user, password, database string, pollTimeoutMs int) ([]ExecResult, error) {
	args := []string{
		"--queries", queriesFile,
		"--log-file", logFile,
		"--host", host,
		"--port", strconv.Itoa(port),
		"--user", user,
		"--password", password,
		"--database", database,
		"--poll-timeout-ms", strconv.Itoa(pollTimeoutMs),
	}
	cmd := exec.Command(executorBin, args...)
	cmd.Stderr = os.Stderr // pass executor's own diagnostics through

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("stdout pipe: %w", err)
	}

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start executor: %w", err)
	}

	var results []ExecResult
	scanner := bufio.NewScanner(stdout)
	for scanner.Scan() {
		line := scanner.Text()
		parts := strings.SplitN(line, "\t", 3)
		if len(parts) != 3 {
			log.Printf("[warn] unexpected executor output line: %q", line)
			continue
		}
		node := parts[0]
		sqlcomInt, err := strconv.Atoi(parts[1])
		if err != nil {
			sqlcomInt = -1
		}
		status := parts[2]
		results = append(results, ExecResult{Node: node, Sqlcom: sqlcomInt, Status: status})
	}
	if err := scanner.Err(); err != nil && err != io.EOF {
		log.Printf("[warn] reading executor stdout: %v", err)
	}

	if err := cmd.Wait(); err != nil {
		// Non-zero exit is logged but not fatal; partial results are still useful.
		log.Printf("[warn] executor exited: %v", err)
	}
	return results, nil
}

// ---------------------------------------------------------------------------
// Mapping construction
// ---------------------------------------------------------------------------

func buildMapping(results []ExecResult, roots []string) []MappingEntry {
	// Accumulate per-node statistics.
	statsMap := make(map[string]*NodeStats)
	for _, r := range roots {
		statsMap[r] = &NodeStats{
			SqlcomFreq: make(map[int]int),
			StatusFreq: make(map[string]int),
		}
	}

	for _, res := range results {
		s, ok := statsMap[res.Node]
		if !ok {
			s = &NodeStats{SqlcomFreq: make(map[int]int), StatusFreq: make(map[string]int)}
			statsMap[res.Node] = s
		}
		s.TotalQueries++
		s.StatusFreq[res.Status]++
		if res.Status == "normal" {
			s.ValidQueries++
			if res.Sqlcom >= 0 {
				s.SqlcomFreq[res.Sqlcom]++
			}
		}
	}

	// Build MappingEntry for each node.
	var entries []MappingEntry
	for _, root := range roots {
		s := statsMap[root]
		if s == nil {
			continue
		}

		// Collect unique sqlcom values.
		var sqlcomValues []int
		for v := range s.SqlcomFreq {
			sqlcomValues = append(sqlcomValues, v)
		}
		sort.Ints(sqlcomValues)

		var sqlcomNamesSlice []string
		for _, v := range sqlcomValues {
			sqlcomNamesSlice = append(sqlcomNamesSlice, sqlcomName(v))
		}

		// Determine dominant sqlcom (highest frequency).
		dominantSqlcom := -1
		dominantCount := 0
		for v, cnt := range s.SqlcomFreq {
			if cnt > dominantCount {
				dominantCount = cnt
				dominantSqlcom = v
			}
		}

		entries = append(entries, MappingEntry{
			Node:               root,
			SqlcomValues:       sqlcomValues,
			SqlcomNames:        sqlcomNamesSlice,
			DominantSqlcom:     dominantSqlcom,
			DominantSqlcomName: sqlcomName(dominantSqlcom),
			Stats:              *s,
		})
	}

	sort.Slice(entries, func(i, j int) bool {
		return entries[i].Node < entries[j].Node
	})
	return entries
}

// buildSqlcomToNodes builds a reverse index: sqlcom_name → []node.
func buildSqlcomToNodes(entries []MappingEntry) map[string][]string {
	rev := make(map[string][]string)
	for _, e := range entries {
		if e.DominantSqlcom < 0 {
			continue
		}
		name := e.DominantSqlcomName
		rev[name] = append(rev[name], e.Node)
	}
	// Sort node lists for determinism.
	for k := range rev {
		sort.Strings(rev[k])
	}
	return rev
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

func main() {
	grammarFile := flag.String("grammar", "../parser_def_files/mysql_sql.y", "MySQL .y grammar file")
	executorBin := flag.String("executor", "./executor/executor", "Path to compiled C++ executor binary")
	logFile := flag.String("log-file", "/tmp/mysql_stderr.log", "mysqld stderr log file (SQLCOM_EXEC output)")
	outputFile := flag.String("output", "./node_sqlcom_mapping.json", "Output JSON mapping file")
	nQueries := flag.Int("n", 200, "SQL queries to generate per node")
	depth := flag.Int("depth", 2, "RSG generation depth")
	seed := flag.Int64("seed", 0, "Random seed (0 = use current time)")
	queriesFile := flag.String("queries-file", "./queries.tsv", "Intermediate queries TSV file")
	extraRoots := flag.String("extra-roots", "", "Comma-separated extra root nodes to probe")
	host := flag.String("host", "127.0.0.1", "MySQL host")
	port := flag.Int("port", 3306, "MySQL port")
	user := flag.String("user", "root", "MySQL user")
	password := flag.String("password", "", "MySQL password")
	database := flag.String("database", "node_mapper_db", "MySQL database name (created/dropped per query)")
	pollTimeoutMs := flag.Int("poll-timeout-ms", 100, "Max ms to wait for SQLCOM_EXEC in mysqld stderr log")
	flag.Parse()

	if *seed == 0 {
		*seed = time.Now().UnixNano()
	}

	// -----------------------------------------------------------------------
	// 1. Load and parse the MySQL grammar.
	// -----------------------------------------------------------------------
	log.Printf("[info] loading grammar: %s", *grammarFile)
	grammarBytes, err := os.ReadFile(*grammarFile)
	if err != nil {
		log.Fatalf("cannot read grammar file: %v", err)
	}

	tree, err := yacc.Parse("sql", string(grammarBytes), "mysql")
	if err != nil {
		log.Fatalf("grammar parse error: %v", err)
	}
	log.Printf("[info] grammar loaded: %d production rules", len(tree.Productions))

	// -----------------------------------------------------------------------
	// 2. Build SimpleRSG.
	// -----------------------------------------------------------------------
	rsg := NewSimpleRSG(tree, *seed)
	log.Printf("[info] SimpleRSG ready: %d unique rule names", len(rsg.allProds))

	// -----------------------------------------------------------------------
	// 3. Collect candidate root nodes.
	// -----------------------------------------------------------------------
	// Primary: all single-token alternatives of simple_statement.
	roots := directChildren(rsg.allProds, "simple_statement")
	log.Printf("[info] found %d direct children of simple_statement", len(roots))

	// Append any caller-supplied extra roots.
	if *extraRoots != "" {
		for _, er := range strings.Split(*extraRoots, ",") {
			er = strings.TrimSpace(er)
			if er != "" {
				roots = append(roots, er)
			}
		}
	}
	// Deduplicate while preserving order.
	seen := make(map[string]bool)
	var dedupRoots []string
	for _, r := range roots {
		if !seen[r] {
			seen[r] = true
			dedupRoots = append(dedupRoots, r)
		}
	}
	roots = dedupRoots
	log.Printf("[info] total root nodes to probe: %d", len(roots))

	// -----------------------------------------------------------------------
	// 4. Generate SQL queries.
	// -----------------------------------------------------------------------
	log.Printf("[info] generating %d queries per node (depth=%d, seed=%d) …", *nQueries, *depth, *seed)
	// Re-seed with a fresh source so each node probing run is independent.
	rsg.rng = rand.New(rand.NewSource(*seed))
	records := generateQueries(rsg, roots, *nQueries, *depth)
	log.Printf("[info] total queries generated: %d", len(records))

	if err := writeQueriesFile(*queriesFile, records); err != nil {
		log.Fatalf("cannot write queries file: %v", err)
	}
	log.Printf("[info] queries written to: %s", *queriesFile)

	// -----------------------------------------------------------------------
	// 5. Run C++ executor.
	// -----------------------------------------------------------------------
	log.Printf("[info] running executor: %s", *executorBin)
	results, err := runExecutor(
		*executorBin, *queriesFile, *logFile,
		*host, *port, *user, *password, *database,
		*pollTimeoutMs,
	)
	if err != nil {
		log.Fatalf("executor error: %v", err)
	}
	log.Printf("[info] executor returned %d results", len(results))

	// -----------------------------------------------------------------------
	// 6. Build and write the JSON mapping.
	// -----------------------------------------------------------------------
	entries := buildMapping(results, roots)
	sqlcomToNodes := buildSqlcomToNodes(entries)

	out := OutputJSON{
		GeneratedAt:    time.Now().Format(time.RFC3339),
		GrammarFile:    *grammarFile,
		QueriesPerNode: *nQueries,
		Mappings:       entries,
		SqlcomToNodes:  sqlcomToNodes,
	}

	jsonBytes, err := json.MarshalIndent(out, "", "  ")
	if err != nil {
		log.Fatalf("JSON serialisation error: %v", err)
	}
	if err := os.WriteFile(*outputFile, jsonBytes, 0644); err != nil {
		log.Fatalf("cannot write output file: %v", err)
	}
	log.Printf("[info] mapping written to: %s", *outputFile)

	// Print a brief summary to stdout.
	fmt.Println("\n=== Node → SQL Command Mapping Summary ===")
	for _, e := range entries {
		if e.DominantSqlcom >= 0 {
			fmt.Printf("  %-45s → %s\n", e.Node, e.DominantSqlcomName)
		}
	}
	fmt.Println()
	fmt.Printf("Full mapping written to: %s\n", *outputFile)
}
