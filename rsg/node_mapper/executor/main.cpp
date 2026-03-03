/**
 * executor/main.cpp
 *
 * C++ MySQL executor for the node_mapper tool.
 *
 * Reads SQL queries from a TSV file (format: node<TAB>sql\n),
 * executes each against a running MySQL instance, and captures the
 * SQLCOM_EXEC integer written by the patched mysqld to its stderr log.
 *
 * Output (stdout): one line per query
 *   node<TAB>sqlcom_int<TAB>status\n
 *
 * status values: normal | syntax_error | semantic_error | crash | no_sqlcom
 *
 * The SQLCOM_EXEC value is extracted from the mysqld stderr log file by:
 *  1. Recording the file size before execution.
 *  2. Executing the SQL query.
 *  3. Polling the file for new content (up to --poll-timeout-ms milliseconds).
 *  4. Scanning new content for lines matching "SQLCOM_EXEC: <int>".
 *
 * Build:
 *   g++ -std=c++17 -O2 -o executor main.cpp \
 *       -I/usr/include/mysql -lmysqlclient -lpthread
 *
 * Usage:
 *   ./executor --queries <queries.tsv> --log-file <mysqld_stderr.log>
 *              [--host 127.0.0.1] [--port 3306]
 *              [--user root] [--password ""] [--database node_mapper_db]
 *              [--poll-timeout-ms 100] [--poll-interval-us 2000]
 */

#include <mysql/mysql.h>
#include <mysql/mysqld_error.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ============================================================
// Utility: parse CLI args
// ============================================================
struct Config {
    std::string queriesFile   = "./queries.tsv";
    std::string logFile       = "/tmp/mysql_stderr.log";
    std::string host          = "127.0.0.1";
    int         port          = 3306;
    std::string user          = "root";
    std::string password      = "";
    std::string database      = "node_mapper_db";
    int         pollTimeoutMs = 100;   // ms to wait for SQLCOM_EXEC line
    int         pollIntervalUs= 2000;  // µs between polls
};

static void printUsage(const char *prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --queries FILE        TSV queries file (node\\tSQL)\n"
              << "  --log-file FILE       mysqld stderr log file\n"
              << "  --host HOST           MySQL host (default 127.0.0.1)\n"
              << "  --port PORT           MySQL port (default 3306)\n"
              << "  --user USER           MySQL user (default root)\n"
              << "  --password PASS       MySQL password (default empty)\n"
              << "  --database DB         Working database (default node_mapper_db)\n"
              << "  --poll-timeout-ms N   Max ms to wait for SQLCOM output (default 100)\n"
              << "  --poll-interval-us N  Polling interval in µs (default 2000)\n";
}

static Config parseArgs(int argc, char **argv) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        std::string key = argv[i];
        if (key == "--queries" && i + 1 < argc)          cfg.queriesFile    = argv[++i];
        else if (key == "--log-file" && i + 1 < argc)    cfg.logFile        = argv[++i];
        else if (key == "--host" && i + 1 < argc)        cfg.host           = argv[++i];
        else if (key == "--port" && i + 1 < argc)        cfg.port           = std::stoi(argv[++i]);
        else if (key == "--user" && i + 1 < argc)        cfg.user           = argv[++i];
        else if (key == "--password" && i + 1 < argc)    cfg.password       = argv[++i];
        else if (key == "--database" && i + 1 < argc)    cfg.database       = argv[++i];
        else if (key == "--poll-timeout-ms" && i + 1 < argc)   cfg.pollTimeoutMs  = std::stoi(argv[++i]);
        else if (key == "--poll-interval-us" && i + 1 < argc)  cfg.pollIntervalUs = std::stoi(argv[++i]);
        else if (key == "--help" || key == "-h") { printUsage(argv[0]); exit(0); }
        else { std::cerr << "Unknown option: " << key << "\n"; printUsage(argv[0]); exit(1); }
    }
    return cfg;
}

// ============================================================
// MySQL connection helpers  (mirrors the reference code)
// ============================================================
enum ExecutionStatus {
    kNormal,
    kServerCrash,
    kSyntaxError,
    kSemanticError
};

static bool isCrashResponse(int response) {
    return response == CR_SERVER_LOST || response == CR_SERVER_GONE_ERROR;
}

static ExecutionStatus cleanUpConnection(MYSQL &mm) {
    int res = -1;
    do {
        MYSQL_RES *q_result = mysql_store_result(&mm);
        if (q_result) mysql_free_result(q_result);
    } while ((res = mysql_next_result(&mm)) == 0);

    if (res != -1) {
        res = mysql_errno(&mm);
        if (isCrashResponse(res)) return kServerCrash;
        if (res == ER_PARSE_ERROR) return kSyntaxError;
        return kSemanticError;
    }
    return kNormal;
}

static bool createDatabase(const std::string &db, const Config &cfg) {
    MYSQL tmp;
    if (!mysql_init(&tmp)) return false;
    if (!mysql_real_connect(&tmp, cfg.host.c_str(), cfg.user.c_str(),
                            cfg.password.c_str(), nullptr, cfg.port,
                            nullptr, CLIENT_MULTI_STATEMENTS)) {
        fprintf(stderr, "[executor] DB create connect error: %s\n", mysql_error(&tmp));
        mysql_close(&tmp);
        return false;
    }
    std::string cmd = "CREATE DATABASE IF NOT EXISTS `" + db + "`;";
    mysql_real_query(&tmp, cmd.c_str(), cmd.size());
    cleanUpConnection(tmp);
    mysql_close(&tmp);
    return true;
}

static bool dropDatabase(const std::string &db, const Config &cfg) {
    MYSQL tmp;
    if (!mysql_init(&tmp)) return false;
    if (!mysql_real_connect(&tmp, cfg.host.c_str(), cfg.user.c_str(),
                            cfg.password.c_str(), nullptr, cfg.port,
                            nullptr, CLIENT_MULTI_STATEMENTS)) {
        mysql_close(&tmp);
        return false;
    }
    std::string cmd = "DROP DATABASE IF EXISTS `" + db + "`;";
    mysql_real_query(&tmp, cmd.c_str(), cmd.size());
    cleanUpConnection(tmp);
    mysql_close(&tmp);
    return true;
}

// ============================================================
// SQLCOM capture helpers
// ============================================================

// Returns the current byte-size of the log file (0 if the file does not exist).
static long getFileSize(const std::string &path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) return 0;
    return static_cast<long>(f.tellg());
}

/**
 * readNewContent reads bytes [offset, EOF) from the log file and returns them
 * as a string.  Returns empty string if nothing new is available.
 */
static std::string readNewContent(const std::string &path, long offset) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    f.seekg(offset, std::ios::beg);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/**
 * parseSqlcom scans text for the first occurrence of "SQLCOM_EXEC: <int>"
 * and returns the integer.  Returns -1 if not found.
 */
static int parseSqlcom(const std::string &text) {
    static const std::string marker = "SQLCOM_EXEC: ";
    std::size_t pos = text.find(marker);
    while (pos != std::string::npos) {
        std::size_t numStart = pos + marker.size();
        std::size_t numEnd   = numStart;
        while (numEnd < text.size() && std::isdigit((unsigned char)text[numEnd]))
            numEnd++;
        if (numEnd > numStart) {
            try {
                return std::stoi(text.substr(numStart, numEnd - numStart));
            } catch (...) {}
        }
        pos = text.find(marker, numStart);
    }
    return -1;
}

/**
 * waitForSqlcom polls the log file for up to timeoutMs milliseconds after
 * offset, looking for a SQLCOM_EXEC line.  Returns the value or -1 on timeout.
 */
static int waitForSqlcom(const std::string &logPath, long offset,
                          int timeoutMs, int pollIntervalUs) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string newContent = readNewContent(logPath, offset);
        if (!newContent.empty()) {
            int v = parseSqlcom(newContent);
            if (v >= 0) return v;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(pollIntervalUs));
    }
    return -1;
}

// ============================================================
// Query record
// ============================================================
struct QueryRecord {
    std::string node;
    std::string sql;
};

static std::vector<QueryRecord> loadQueries(const std::string &path) {
    std::vector<QueryRecord> records;
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "[executor] cannot open queries file: %s\n", path.c_str());
        return records;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        QueryRecord r;
        r.node = line.substr(0, tab);
        r.sql  = line.substr(tab + 1);
        records.push_back(std::move(r));
    }
    return records;
}

// ============================================================
// Execute one SQL and return (sqlcom, status_string)
// ============================================================
static std::pair<int, std::string>
executeOne(const std::string &sql, const Config &cfg, MYSQL &conn,
           bool &connected, int &crashCount) {

    // Reconnect if needed (e.g. after a crash).
    if (!connected) {
        // Retry with backoff — mysqld may need a few seconds to restart.
        const int maxRetries = 30;
        const int retryDelayMs = 1000;
        bool ok = false;
        for (int attempt = 0; attempt < maxRetries; attempt++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
            if (createDatabase(cfg.database, cfg)) {
                if (mysql_real_connect(&conn, cfg.host.c_str(), cfg.user.c_str(),
                                       cfg.password.c_str(), cfg.database.c_str(),
                                       cfg.port, nullptr, CLIENT_MULTI_STATEMENTS)) {
                    ok = true;
                    break;
                }
                mysql_close(&conn);
                mysql_init(&conn);
            }
            fprintf(stderr, "[executor] reconnect attempt %d/%d failed, retrying...\n",
                    attempt + 1, maxRetries);
        }
        if (!ok) {
            fprintf(stderr, "[executor] could not reconnect after %d attempts, giving up on this query\n", maxRetries);
            return {-1, "no_sqlcom"};
        }
        connected = true;
        fprintf(stderr, "[executor] reconnected to MySQL\n");
    }

    // Sample log file position before execution.
    long logOffset = getFileSize(cfg.logFile);

    // Execute.
    int serverResp = mysql_real_query(&conn, sql.c_str(), sql.size());
    // Capture the error number BEFORE cleanUpConnection, because mysql_next_result
    // returning -1 (no more results after a failed single-statement query) will
    // reset errno to 0, causing crash errors to go undetected.
    unsigned int errnoAfterQuery = mysql_errno(&conn);
    ExecutionStatus status = cleanUpConnection(conn);
    // If cleanUpConnection didn't detect a crash but mysql_real_query itself
    // returned a connection-loss error, honour that.
    if (status == kNormal && isCrashResponse(static_cast<int>(errnoAfterQuery))) {
        status = kServerCrash;
    }

    std::string statusStr;
    int sqlcom = -1;

    switch (status) {
        case kServerCrash:
            statusStr = "crash";
            connected = false;
            crashCount++;
            // Reinitialise the connection handle for the next attempt.
            mysql_close(&conn);
            mysql_init(&conn);
            // On crash mysqld won't emit SQLCOM_EXEC, so skip polling.
            return {-1, statusStr};

        case kSyntaxError:
            statusStr = "syntax_error";
            break;

        case kSemanticError:
            statusStr = "semantic_error";
            break;

        case kNormal:
            statusStr = "normal";
            break;
    }
    (void)serverResp;

    // Wait for SQLCOM_EXEC in the log (only meaningful on normal/semantic exec).
    if (status == kNormal || status == kSemanticError) {
        sqlcom = waitForSqlcom(cfg.logFile, logOffset,
                               cfg.pollTimeoutMs, cfg.pollIntervalUs);
        if (sqlcom < 0) {
            if (statusStr == "normal") statusStr = "no_sqlcom";
        }
    }

    return {sqlcom, statusStr};
}

// ============================================================
// main
// ============================================================
int main(int argc, char **argv) {
    Config cfg = parseArgs(argc, argv);

    // ------------------------------------------------------------------
    // Load queries.
    // ------------------------------------------------------------------
    auto records = loadQueries(cfg.queriesFile);
    if (records.empty()) {
        fprintf(stderr, "[executor] no queries loaded from %s\n", cfg.queriesFile.c_str());
        return 1;
    }
    fprintf(stderr, "[executor] loaded %zu queries\n", records.size());

    // ------------------------------------------------------------------
    // Verify/warn about log file.
    // ------------------------------------------------------------------
    {
        std::ifstream testLog(cfg.logFile);
        if (!testLog.is_open()) {
            fprintf(stderr,
                "[executor] WARNING: log file %s does not exist or is not readable.\n"
                "           mysqld must be started with stderr redirected to this file:\n"
                "             mysqld ... 2>>%s\n",
                cfg.logFile.c_str(), cfg.logFile.c_str());
        }
    }

    // ------------------------------------------------------------------
    // Prepare database and initial connection.
    // ------------------------------------------------------------------
    dropDatabase(cfg.database, cfg);
    if (!createDatabase(cfg.database, cfg)) {
        fprintf(stderr, "[executor] cannot create database %s\n", cfg.database.c_str());
        return 1;
    }

    MYSQL conn;
    mysql_init(&conn);
    bool connected = false;
    if (!mysql_real_connect(&conn, cfg.host.c_str(), cfg.user.c_str(),
                             cfg.password.c_str(), cfg.database.c_str(),
                             cfg.port, nullptr, CLIENT_MULTI_STATEMENTS)) {
        fprintf(stderr, "[executor] initial connect failed: %s\n", mysql_error(&conn));
        mysql_close(&conn);
        return 1;
    }
    connected = true;
    fprintf(stderr, "[executor] connected to MySQL %s:%d\n", cfg.host.c_str(), cfg.port);

    // ------------------------------------------------------------------
    // Execute queries, emit results.
    // ------------------------------------------------------------------
    int crashCount  = 0;
    int normalCount = 0;
    int errorCount  = 0;
    std::size_t total = records.size();

    // Use line-buffered stdout so Go can read results in real time.
    setvbuf(stdout, nullptr, _IOLBF, 0);

    for (std::size_t i = 0; i < total; i++) {
        const auto &rec = records[i];

        if ((i + 1) % 500 == 0) {
            fprintf(stderr, "[executor] progress: %zu/%zu (crashes: %d)\n",
                    i + 1, total, crashCount);
        }

        auto [sqlcom, statusStr] = executeOne(rec.sql, cfg, conn, connected, crashCount);

        // Emit result line.
        printf("%s\t%d\t%s\n", rec.node.c_str(), sqlcom, statusStr.c_str());

        if (statusStr == "normal")       normalCount++;
        else if (statusStr == "crash")   {}
        else                             errorCount++;
    }

    // ------------------------------------------------------------------
    // Cleanup.
    // ------------------------------------------------------------------
    if (connected) mysql_close(&conn);
    dropDatabase(cfg.database, cfg);

    fprintf(stderr,
            "[executor] done: %zu queries, %d normal, %d errors, %d crashes\n",
            total, normalCount, errorCount, crashCount);
    return 0;
}
