#!/usr/bin/env bash
# test-static-bins.sh — Static binary smoke tests for hl
#
# Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Tests a variety of standalone static aarch64-linux-musl binaries through hl.
# Exercises different runtime profiles: shell interpreters (bash, dash),
# scripting languages (lua, gawk), text tools (grep, sed, find), and
# compute-heavy workloads (fibonacci, mandelbrot).
#
# Usage: test/test-static-bins.sh <hl-binary> <static-bins-dir>
#        where <static-bins-dir> contains: bash, dash, lua, gawk, grep, sed,
#        find, tree, jq, sqlite3, diffutils/diff

set -euo pipefail

HL="${1:?Usage: $0 <hl-binary> <static-bins-dir>}"
BINDIR="${2:?Usage: $0 <hl-binary> <static-bins-dir>}"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RESET='\033[0m'

pass=0
fail=0
skip=0

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Resolve binary path: try $BINDIR/<name> then $BINDIR/bin/<name>.
# Uses -f (not -x) because aarch64-linux ELFs aren't executable on macOS.
find_bin() {
    local name="$1"
    if [ -f "$BINDIR/$name" ]; then
        echo "$BINDIR/$name"
    elif [ -f "$BINDIR/bin/$name" ]; then
        echo "$BINDIR/bin/$name"
    else
        echo ""
    fi
}

# Run a test, checking that output contains a pattern
run_check() {
    local label="$1"; shift
    local bin="$1"; shift
    local pattern="$1"; shift
    local name
    name=$(printf "%-24s" "$label")

    if [ -z "$bin" ]; then
        printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (binary not found)\n" "$name"
        skip=$((skip + 1))
        return
    fi

    if output=$("$HL" "$bin" "$@" 2>&1); then
        rc=0
    else
        rc=$?
    fi

    if echo "$output" | grep -qE "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (pattern '%s' not found, rc=%d)\n" "$name" "$pattern" "$rc"
        printf "  %.120s\n" "$output" | head -3
        fail=$((fail + 1))
    fi
}

# Run a test with piped stdin
run_pipe() {
    local label="$1"; shift
    local bin="$1"; shift
    local pattern="$1"; shift
    local input="$1"; shift
    local name
    name=$(printf "%-24s" "$label")

    if [ -z "$bin" ]; then
        printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (binary not found)\n" "$name"
        skip=$((skip + 1))
        return
    fi

    if output=$(printf '%s' "$input" | "$HL" "$bin" "$@" 2>&1); then
        rc=0
    else
        rc=$?
    fi

    if echo "$output" | grep -qE "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (pattern '%s' not found, rc=%d)\n" "$name" "$pattern" "$rc"
        printf "  %.120s\n" "$output" | head -3
        fail=$((fail + 1))
    fi
}

# Run a test with a script file
run_script() {
    local label="$1"; shift
    local bin="$1"; shift
    local pattern="$1"; shift
    local script_file="$1"; shift
    local name
    name=$(printf "%-24s" "$label")

    if [ -z "$bin" ]; then
        printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (binary not found)\n" "$name"
        skip=$((skip + 1))
        return
    fi

    if output=$("$HL" "$bin" "$@" "$script_file" 2>&1); then
        rc=0
    else
        rc=$?
    fi

    if echo "$output" | grep -qE "$pattern"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (pattern '%s' not found, rc=%d)\n" "$name" "$pattern" "$rc"
        printf "  %.120s\n" "$output" | head -3
        fail=$((fail + 1))
    fi
}

# Locate binaries
BASH_BIN=$(find_bin bash)
DASH_BIN=$(find_bin dash)
LUA_BIN=$(find_bin lua)
GAWK_BIN=$(find_bin gawk)
GREP_BIN=$(find_bin grep)
SED_BIN=$(find_bin sed)
FIND_BIN=$(find_bin find)
TREE_BIN=$(find_bin tree)
JQ_BIN=$(find_bin jq)
SQLITE_BIN=$(find_bin sqlite3)
DIFF_BIN=$(find_bin diff)

# Test fixtures
echo "hello world" > "$TMPDIR/hello.txt"
echo -e "cherry\napple\nbanana\ndate\nelderberry" > "$TMPDIR/fruits.txt"
mkdir -p "$TMPDIR/tree/a/b/c" "$TMPDIR/tree/a/d" "$TMPDIR/tree/e"
echo "content1" > "$TMPDIR/tree/a/b/file1.txt"
echo "content2" > "$TMPDIR/tree/a/b/c/deep.txt"
echo "content3" > "$TMPDIR/tree/a/d/file2.txt"
echo "content4" > "$TMPDIR/tree/e/file3.txt"

printf "\n${BLUE}━━━ Static binary smoke tests ━━━${RESET}\n\n"

# ── Dash shell ────────────────────────────────────────────────────
printf "${BLUE}── Dash shell ──${RESET}\n"
run_check  "dash: echo"         "$DASH_BIN" "hello"             -c "echo hello"
run_check  "dash: arithmetic"   "$DASH_BIN" "2\\+3=5"           -c 'echo "2+3=$((2+3))"'
run_check  "dash: loop"         "$DASH_BIN" "count=10"          -c 'i=0; while [ $i -lt 10 ]; do i=$((i+1)); done; echo "count=$i"'
run_check  "dash: conditionals" "$DASH_BIN" "5>3 OK"            -c 'if [ 5 -gt 3 ]; then echo "5>3 OK"; fi'
run_check  "dash: case"         "$DASH_BIN" "glob match"        -c 'case "hello" in hel*) echo "glob match" ;; esac'
run_check  "dash: functions"    "$DASH_BIN" "result=120"        -c 'fact() { [ "$1" -le 1 ] && echo 1 && return; echo $(( $1 * $(fact $(($1 - 1))) )); }; echo "result=$(fact 5)"'

# ── Bash shell ────────────────────────────────────────────────────
printf "\n${BLUE}── Bash shell ──${RESET}\n"

# Write bash test script (avoids shell escaping issues with -c)
cat > "$TMPDIR/bash-test.sh" << 'BASH_SCRIPT'
echo "bash ${BASH_VERSION}"
echo "sum=$((17 * 31))"
arr=(alpha beta gamma delta)
echo "array=${#arr[@]}"
echo "upper=${arr[0]^^}"
declare -A map
map[k]="value"
echo "assoc=${map[k]}"
if [[ "abc123" =~ [0-9]+ ]]; then echo "regex=${BASH_REMATCH[0]}"; fi
printf "printf=%05d\n" 42
echo "done"
BASH_SCRIPT
run_check  "bash: version"      "$BASH_BIN" "bash 5"            "$TMPDIR/bash-test.sh"
run_check  "bash: arithmetic"   "$BASH_BIN" "sum=527"           "$TMPDIR/bash-test.sh"
run_check  "bash: arrays"       "$BASH_BIN" "array=4"           "$TMPDIR/bash-test.sh"
run_check  "bash: uppercase"    "$BASH_BIN" "upper=ALPHA"       "$TMPDIR/bash-test.sh"
run_check  "bash: assoc arrays" "$BASH_BIN" "assoc=value"       "$TMPDIR/bash-test.sh"
run_check  "bash: regex"        "$BASH_BIN" "regex=123"         "$TMPDIR/bash-test.sh"
run_check  "bash: printf"       "$BASH_BIN" "printf=00042"      "$TMPDIR/bash-test.sh"

# Bash subshell (exercises fork)
run_check  "bash: subshell"     "$BASH_BIN" "sub=25"            -c 'echo "sub=$(echo $((5*5)))"'

# ── Lua interpreter ──────────────────────────────────────────────
printf "\n${BLUE}── Lua interpreter ──${RESET}\n"
run_check  "lua: hello"         "$LUA_BIN" "Hello.*Lua"         -e 'print("Hello from " .. _VERSION)'
run_check  "lua: arithmetic"    "$LUA_BIN" "3628800"            -e 'local r=1; for i=2,10 do r=r*i end; print(r)'

# Lua sieve of Eratosthenes (compute heavy)
cat > "$TMPDIR/sieve.lua" << 'LUA_SIEVE'
local function sieve(n)
  local is_prime = {}
  for i = 2, n do is_prime[i] = true end
  for i = 2, math.floor(math.sqrt(n)) do
    if is_prime[i] then
      for j = i*i, n, i do is_prime[j] = false end
    end
  end
  local count = 0
  for i = 2, n do if is_prime[i] then count = count + 1 end end
  return count
end
print("primes=" .. sieve(100000))
LUA_SIEVE
run_script "lua: sieve(100000)"  "$LUA_BIN" "primes=9592"       "$TMPDIR/sieve.lua"

# Lua fibonacci (recursion stress)
run_check  "lua: fib(30)"       "$LUA_BIN" "832040"             -e 'local function f(n) if n<2 then return n end; return f(n-1)+f(n-2) end; print(f(30))'

# Lua string processing
run_check  "lua: strings"       "$LUA_BIN" "length=1000"        -e 'local s=""; for i=1,1000 do s=s..string.char(65+(i%26)) end; print("length=" .. #s)'

# ── GNU awk ──────────────────────────────────────────────────────
printf "\n${BLUE}── GNU awk ──${RESET}\n"
run_pipe   "gawk: field split"    "$GAWK_BIN" "world"           "hello world" '{print $2}'
run_pipe   "gawk: arithmetic"     "$GAWK_BIN" "Average: 90"     "$(printf 'Alice 90\nBob 85\nCharlie 95')" '{sum+=$2; n++} END{print "Average:", sum/n}'

# Gawk mandelbrot (compute heavy)
cat > "$TMPDIR/mandel.awk" << 'AWK_MANDEL'
BEGIN {
    rows = 0
    for (y = -1.0; y <= 1.0; y += 0.1) {
        line = ""
        for (x = -2.0; x <= 0.6; x += 0.05) {
            zr = 0; zi = 0; i = 0
            while (i < 50 && zr*zr + zi*zi < 4) {
                tmp = zr*zr - zi*zi + x
                zi = 2*zr*zi + y
                zr = tmp; i++
            }
            if (i >= 50) line = line "#"
            else line = line " "
        }
        rows++
    }
    print "mandelbrot rows=" rows
}
AWK_MANDEL
run_script "gawk: mandelbrot"     "$GAWK_BIN" "mandelbrot rows=21" "$TMPDIR/mandel.awk" -f

# ── GNU grep ─────────────────────────────────────────────────────
printf "\n${BLUE}── GNU grep ──${RESET}\n"
run_pipe   "grep: basic"          "$GREP_BIN" "hello"           "hello world" "hello"
run_pipe   "grep: regex"          "$GREP_BIN" "brown"           "The quick brown fox" -o 'brown\|lazy'
run_pipe   "grep: count"          "$GREP_BIN" "^3$"             "$(printf 'a\nb\nc')" -c "."
run_pipe   "grep: invert"         "$GREP_BIN" "banana"          "$(printf 'apple\nbanana\ncherry')" -v "a..le\|c..rry"
run_check  "grep: file"           "$GREP_BIN" "cherry"          "cherry" "$TMPDIR/fruits.txt"

# ── GNU sed ──────────────────────────────────────────────────────
printf "\n${BLUE}── GNU sed ──${RESET}\n"
run_pipe   "sed: substitute"      "$SED_BIN" "HELLO"            "hello" 's/hello/HELLO/'
run_pipe   "sed: delete"          "$SED_BIN" "^banana$"         "$(printf 'apple\nbanana\ncherry')" '/apple/d'
run_pipe   "sed: numbering"       "$SED_BIN" "^2$"              "$(printf 'hello\nworld')" '='
run_pipe   "sed: multi-cmd"       "$SED_BIN" "earth XXX"        "hello world 123" 's/world/earth/; s/[0-9]/X/g'

# ── GNU find ─────────────────────────────────────────────────────
printf "\n${BLUE}── GNU find ──${RESET}\n"
run_check  "find: by name"        "$FIND_BIN" "file1.txt"       "$TMPDIR/tree" -name "file1.txt"
run_check  "find: by type"        "$FIND_BIN" "/a$"             "$TMPDIR/tree" -type d
run_check  "find: all txt"        "$FIND_BIN" "deep.txt"        "$TMPDIR/tree" -name "*.txt"

# ── tree ─────────────────────────────────────────────────────────
printf "\n${BLUE}── tree ──${RESET}\n"
run_check  "tree: render"         "$TREE_BIN" "file1.txt"       "$TMPDIR/tree"
run_check  "tree: summary"        "$TREE_BIN" "director"        "$TMPDIR/tree"

# ── jq ───────────────────────────────────────────────────────────
printf "\n${BLUE}── jq ──${RESET}\n"
run_pipe   "jq: simple"           "$JQ_BIN" "^1$"               '{"a":1}' '.a'
run_pipe   "jq: array"            "$JQ_BIN" "^3$"               '[1,2,3]' '.[2]'
run_pipe   "jq: filter"           "$JQ_BIN" "Alice"             '{"users":[{"name":"Alice","age":30},{"name":"Bob","age":25}]}' '.users[] | select(.age > 28) | .name'
run_pipe   "jq: map"              "$JQ_BIN" "\\[2,4,6\\]"      '[1,2,3]' -c '[.[] * 2]'
run_pipe   "jq: keys"             "$JQ_BIN" '"b"'               '{"a":1,"b":2,"c":3}' 'keys[1]'

# ── SQLite ───────────────────────────────────────────────────────
printf "\n${BLUE}── SQLite ──${RESET}\n"
run_check  "sqlite: version"      "$SQLITE_BIN" "^3\\."         ":memory:" "SELECT sqlite_version();"
run_check  "sqlite: arithmetic"   "$SQLITE_BIN" "^42$"          ":memory:" "SELECT 6 * 7;"
run_check  "sqlite: strings"      "$SQLITE_BIN" "HELLO"         ":memory:" "SELECT upper('hello');"

# SQLite table operations
cat > "$TMPDIR/sqlite-test.sql" << 'SQL'
CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL);
INSERT INTO t VALUES (1, 'Alice', 95.5);
INSERT INTO t VALUES (2, 'Bob', 87.3);
INSERT INTO t VALUES (3, 'Charlie', 92.1);
SELECT name || ': ' || score FROM t WHERE score > 90 ORDER BY score DESC;
SQL
run_check  "sqlite: table ops"    "$SQLITE_BIN" "Alice.*95.5"   ":memory:" ".read $TMPDIR/sqlite-test.sql"

# SQLite aggregate
run_check  "sqlite: aggregate"    "$SQLITE_BIN" "^55$"          ":memory:" "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c WHERE x<10) SELECT sum(x) FROM c;"

# ── diff ─────────────────────────────────────────────────────────
printf "\n${BLUE}── diff ──${RESET}\n"
echo "different content" > "$TMPDIR/other.txt"
# diff returns 1 when files differ (not an error)
label="diff: different files"
name=$(printf "%-24s" "$label")
if [ -n "$DIFF_BIN" ]; then
    if output=$("$HL" "$DIFF_BIN" "$TMPDIR/hello.txt" "$TMPDIR/other.txt" 2>&1); then
        rc=0
    else
        rc=$?
    fi
    if [ "$rc" = "1" ] && echo "$output" | grep -qE "^[<>]"; then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (rc=%d)\n" "$name" "$rc"
        printf "  %.120s\n" "$output" | head -3
        fail=$((fail + 1))
    fi
else
    printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (binary not found)\n" "$name"
    skip=$((skip + 1))
fi

# diff: identical files (exit code 0, no content diff output)
label="diff: identical"
name=$(printf "%-24s" "$label")
if [ -n "$DIFF_BIN" ]; then
    if output=$("$HL" "$DIFF_BIN" "$TMPDIR/hello.txt" "$TMPDIR/hello.txt" 2>&1); then
        printf "${YELLOW}▸${RESET} %s ${GREEN}✓ PASS${RESET}\n" "$name"
        pass=$((pass + 1))
    else
        rc=$?
        printf "${YELLOW}▸${RESET} %s ${RED}✗ FAIL${RESET} (rc=%d, expected 0)\n" "$name" "$rc"
        fail=$((fail + 1))
    fi
else
    printf "${YELLOW}▸${RESET} %s ${BLUE}⊘ SKIP${RESET} (binary not found)\n" "$name"
    skip=$((skip + 1))
fi

# ── Summary ──────────────────────────────────────────────────────
total=$((pass + fail + skip))
printf "\n${BLUE}━━━ Results: %d passed, %d failed, %d skipped (of %d) ━━━${RESET}\n" \
    "$pass" "$fail" "$skip" "$total"

if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
