# Makefile for rsg/node_mapper
#
# Builds two independent targets:
#   1. node_mapper  — Go binary (SQL generator + orchestrator)
#   2. executor/executor  — C++ MySQL client
#
# Run from the rsg/node_mapper/ directory, or invoke as:
#   make -C rsg/node_mapper
#
# Prerequisites:
#   - Go 1.17+  (same as the parent rsg module)
#   - g++ with C++17 support
#   - libmysqlclient-dev  (provides /usr/include/mysql and -lmysqlclient)
#
# ============================================================

# ---- Go settings -----------------------------------------------------------
GO          ?= go
GO_BUILD     = $(GO) build
GO_FLAGS     =
# The node_mapper package lives inside the github.com/rsg module.
# Build from the parent rsg/ directory to pick up the correct go.mod.
RSG_DIR     := $(shell cd .. && pwd)
NM_PKG       = github.com/rsg/node_mapper

NODE_MAPPER_BIN = node_mapper

# ---- C++ settings ----------------------------------------------------------
CXX         ?= g++
CXXFLAGS    = -std=c++17 -O2 -Wall -Wextra -I/usr/include/mysql \
              $(shell pkg-config --cflags mysqlclient 2>/dev/null || echo "-I/usr/include/mysql")
# Use pkg-config when available; fall back to the known lib64 path.
MYSQL_LDFLAGS := $(shell pkg-config --libs mysqlclient 2>/dev/null || echo "-L/usr/lib64/mysql -lmysqlclient")
LDFLAGS     = $(MYSQL_LDFLAGS) -lpthread

EXECUTOR_SRC = executor/main.cpp
EXECUTOR_BIN = executor/executor

# ============================================================
# Default target: build everything
# ============================================================
.PHONY: all
all: $(NODE_MAPPER_BIN) $(EXECUTOR_BIN)

# ============================================================
# Go binary
# ============================================================
$(NODE_MAPPER_BIN): simple_rsg.go main.go
	@echo "==> Building Go node_mapper binary …"
	cd $(RSG_DIR) && $(GO_BUILD) $(GO_FLAGS) -o node_mapper/$(NODE_MAPPER_BIN) ./node_mapper/
	@echo "    -> node_mapper/$(NODE_MAPPER_BIN)"

# ============================================================
# C++ executor binary
# ============================================================
$(EXECUTOR_BIN): $(EXECUTOR_SRC)
	@echo "==> Building C++ executor …"
	$(CXX) $(CXXFLAGS) -o $(EXECUTOR_BIN) $(EXECUTOR_SRC) $(LDFLAGS)
	@echo "    -> $(EXECUTOR_BIN)"

# ============================================================
# Convenience targets
# ============================================================
.PHONY: go
go: $(NODE_MAPPER_BIN)

.PHONY: cpp
cpp: $(EXECUTOR_BIN)

.PHONY: clean
clean:
	@echo "==> Cleaning …"
	rm -f $(NODE_MAPPER_BIN) $(EXECUTOR_BIN)

# ============================================================
# Run the mapping discovery (requires a running MySQL instance)
# ============================================================
# Override any variable on the command line, e.g.:
#   make run LOG_FILE=/var/log/mysql/error.log N=300
# ============================================================
LOG_FILE    ?= /tmp/mysql_stderr.log
GRAMMAR     ?= ../parser_def_files/mysql_sql.y
OUTPUT      ?= ./node_sqlcom_mapping.json
N           ?= 200
DEPTH       ?= 2
MYSQL_HOST  ?= 127.0.0.1
MYSQL_PORT  ?= 3306
MYSQL_USER  ?= root
MYSQL_PASS  ?=
# MYSQL_SOCKET?= /tmp/mysql_node_mapper.sock
MYSQL_DB    ?= node_mapper_db

.PHONY: run
run: all
	@echo "==> Starting node mapping …"
	@echo "    MySQL stderr log: $(LOG_FILE)"
	@echo "    (mysqld must be started with stderr redirected to $(LOG_FILE))"
	./$(NODE_MAPPER_BIN) \
		-grammar  "$(GRAMMAR)"     \
		-executor "./$(EXECUTOR_BIN)" \
		-log-file "$(LOG_FILE)"    \
		-output   "$(OUTPUT)"      \
		-n        "$(N)"           \
		-depth    "$(DEPTH)"       \
		-host     "$(MYSQL_HOST)"  \
		-port     "$(MYSQL_PORT)"  \
		-user     "$(MYSQL_USER)"  \
		-password "$(MYSQL_PASS)"  \
		-database "$(MYSQL_DB)"    

.PHONY: help
help:
	@echo "Targets:"
	@echo "  all          Build node_mapper (Go) and executor (C++)"
	@echo "  go           Build only the Go binary"
	@echo "  cpp          Build only the C++ executor"
	@echo "  run          Build and run the full mapping discovery"
	@echo "  clean        Remove built binaries"
	@echo ""
	@echo "Variables (override on command line):"
	@echo "  LOG_FILE     mysqld stderr log path  (default: /tmp/mysql_stderr.log)"
	@echo "  GRAMMAR      MySQL .y grammar file   (default: ../parser_def_files/mysql_sql.y)"
	@echo "  OUTPUT       JSON output file        (default: ./node_sqlcom_mapping.json)"
	@echo "  N            Queries per node        (default: 200)"
	@echo "  DEPTH        RSG depth               (default: 2)"
	@echo "  MYSQL_HOST   MySQL host              (default: 127.0.0.1)"
	@echo "  MYSQL_PORT   MySQL port              (default: 3306)"
	@echo "  MYSQL_USER   MySQL user              (default: root)"
	@echo "  MYSQL_PASS   MySQL password          (default: empty)"
	@echo "  MYSQL_DB     Working database        (default: node_mapper_db)"
