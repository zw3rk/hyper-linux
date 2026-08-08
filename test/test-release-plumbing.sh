#!/bin/sh
# Regression tests for deterministic release version and artifact handling.

# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)

# shellcheck source=../dist/release-lib.sh
. "$ROOT/dist/release-lib.sh"

failures=0

check_eq() {
    name=$1
    expected=$2
    actual=$3
    if [ "$actual" = "$expected" ]; then
        printf '  PASS %s\n' "$name"
    else
        printf '  FAIL %s: expected %s, got %s\n' "$name" "$expected" "$actual" >&2
        failures=$((failures + 1))
    fi
}

check_rejected() {
    name=$1
    version=$2
    if release_parse_version "$version"; then
        printf '  FAIL %s: accepted %s\n' "$name" "$version" >&2
        failures=$((failures + 1))
    else
        printf '  PASS %s\n' "$name"
    fi
}

check_file_contains() {
    name=$1
    file=$2
    needle=$3
    if grep -Fq -- "$needle" "$file"; then
        printf '  PASS %s\n' "$name"
    else
        printf '  FAIL %s: %s does not contain %s\n' \
            "$name" "$file" "$needle" >&2
        failures=$((failures + 1))
    fi
}

release_parse_version v0.2.4
check_eq 'stable major' 0 "$RELEASE_MAJOR"
check_eq 'stable minor' 2 "$RELEASE_MINOR"
check_eq 'stable patch' 4 "$RELEASE_PATCH"
check_eq 'stable prerelease' '' "$RELEASE_PRERELEASE"
check_eq 'stable next patch' 0.2.5 "$(release_next_patch)"

release_parse_version v0.3.0-rc1
check_eq 'rc base' 0.3.0 "$RELEASE_BASE_VERSION"
check_eq 'rc number' 1 "$RELEASE_RC_NUMBER"
check_eq 'rc successor' 0.3.0-rc2 "$(release_next_rc)"
check_eq 'rc final' 0.3.0 "$(release_final_version)"
if release_is_prerelease 2>/dev/null; then
    printf '  PASS rc classified as prerelease\n'
else
    printf '  FAIL rc classified as prerelease\n' >&2
    failures=$((failures + 1))
fi

release_parse_version v0.3.0
if release_is_prerelease 2>/dev/null; then
    printf '  FAIL stable classified as non-prerelease\n' >&2
    failures=$((failures + 1))
else
    printf '  PASS stable classified as non-prerelease\n'
fi

check_rejected 'missing patch component' v0.3
check_rejected 'nonnumeric component' v0.x.0
check_rejected 'unsupported prerelease' v0.3.0-beta1
check_rejected 'empty rc number' v0.3.0-rc

tmp=$(mktemp -d "${TMPDIR:-/tmp}/hl-release-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
: > "$tmp/hl-v0.2.3.zip"
: > "$tmp/hl-v0.3.0-rc2.zip"
check_eq 'exact zip path' "$tmp/hl-v0.3.0-rc2.zip" \
    "$(release_artifact_path "$tmp" v0.3.0-rc2 zip)"
check_eq 'exact pkg path' "$tmp/hl-v0.3.0-rc2.pkg" \
    "$(release_artifact_path "$tmp" v0.3.0-rc2 pkg)"

# VERSION is a build input, not just an archive label.  Exercise the real
# Makefile in an isolated directory so a version-only change must rewrite the
# generated header even when no Git metadata or source timestamp changed.
mkdir -p "$tmp/make"
cp "$ROOT/Makefile" "$tmp/make/Makefile"
make -s -C "$tmp/make" _build/version.h VERSION=v1.2.3
make -s -C "$tmp/make" _build/version.h VERSION=v1.2.4
make_version=$(sed -n 's/^#define HL_VERSION "\([^"]*\)"/\1/p' \
    "$tmp/make/_build/version.h")
check_eq 'Make VERSION invalidates version.h' v1.2.4 "$make_version"
same_version_dry_run=$(make -n -s -C "$tmp/make" _build/version.h \
    VERSION=v1.2.4)
check_eq 'unchanged Make VERSION stays up to date' '' "$same_version_dry_run"

# The interactive and CI draft creators must preserve RC semantics.  The
# publisher itself is exercised below with mocked gh/git commands.
check_file_contains 'interactive draft marks RC prerelease' \
    "$ROOT/dist/release.sh" '--prerelease'
check_file_contains 'CI fallback marks RC prerelease' \
    "$ROOT/.github/workflows/ci.yml" \
    "prerelease: \${{ contains(github.ref_name, '-rc') }}"

# Build a disposable project plus deterministic gh/git mocks.  This runs the
# real publisher end-to-end without touching a repository, network, or tap.
publisher_root="$tmp/publisher"
mock_bin="$tmp/mock-bin"
mock_remote="$tmp/mock-remote"
mock_log="$tmp/mock-calls.log"
mkdir -p "$publisher_root/dist/out" "$publisher_root/Formula" \
    "$mock_bin" "$mock_remote"
cp "$ROOT/dist/release-lib.sh" "$publisher_root/dist/release-lib.sh"
cp "$ROOT/dist/publish-release.sh" "$publisher_root/dist/publish-release.sh"

cat > "$mock_bin/gh" <<'MOCK_GH'
#!/bin/sh
set -eu
printf 'gh %s\n' "$*" >> "$MOCK_CALL_LOG"
case "$1 $2" in
    'run list')
        printf '%s\tcompleted\tsuccess\thttps://ci.example/run\n' "$MOCK_COMMIT"
        ;;
    'release view')
        case " $* " in
            *' --json isDraft '*) printf 'true\n' ;;
            *' --json assets '*)
                for asset in "$MOCK_REMOTE_DIR"/*; do
                    [ -f "$asset" ] && basename "$asset"
                done
                ;;
            *) exit 2 ;;
        esac
        ;;
    'release download')
        download_dir=
        shift 2
        while [ "$#" -gt 0 ]; do
            case $1 in
                --dir) download_dir=$2; shift 2 ;;
                *) shift ;;
            esac
        done
        [ -n "$download_dir" ]
        mkdir -p "$download_dir"
        for asset in "$MOCK_REMOTE_DIR"/*; do
            [ -f "$asset" ] && cp "$asset" "$download_dir/"
        done
        ;;
    'repo clone')
        mkdir -p "$4/Formula"
        printf '# old formula\n' > "$4/Formula/hl.rb"
        ;;
    'release edit')
        [ "${MOCK_FAIL_PUBLISH:-0}" != 1 ]
        ;;
    *) exit 2 ;;
esac
MOCK_GH

cat > "$mock_bin/git" <<'MOCK_GIT'
#!/bin/sh
set -eu
printf 'git %s\n' "$*" >> "$MOCK_CALL_LOG"
case $1 in
    rev-parse)
        if [ "${2-}" = --abbrev-ref ]; then printf 'master\n';
        else printf '%s\n' "$MOCK_COMMIT"; fi
        ;;
    ls-remote) printf '%s\trefs/tags/v0.3.0^{}\n' "$MOCK_COMMIT" ;;
    add|commit|push|revert) ;;
    diff) exit 1 ;;
    *) exit 2 ;;
esac
MOCK_GIT
chmod +x "$mock_bin/gh" "$mock_bin/git"

prepare_publisher_case() {
    publisher_tag=$1
    publisher_zip="hl-${publisher_tag}.zip"
    publisher_pkg="hl-${publisher_tag}.pkg"
    rm -rf "$publisher_root/dist/out" "$mock_remote"
    mkdir -p "$publisher_root/dist/out" "$mock_remote"
    printf 'local zip for %s\n' "$publisher_tag" > \
        "$publisher_root/dist/out/$publisher_zip"
    printf 'local pkg for %s\n' "$publisher_tag" > \
        "$publisher_root/dist/out/$publisher_pkg"
    (cd "$publisher_root/dist/out" &&
        shasum -a 256 "$publisher_zip" "$publisher_pkg" > SHA256SUMS)
    publisher_sha=$(shasum -a 256 \
        "$publisher_root/dist/out/$publisher_zip" | awk '{ print $1 }')
    {
        printf 'class Hl < Formula\n'
        printf '  url "https://github.com/zw3rk/hyper-linux/releases/download/%s/%s"\n' \
            "$publisher_tag" "$publisher_zip"
        printf '  sha256 "%s"\n' "$publisher_sha"
        printf 'end\n'
    } > "$publisher_root/Formula/hl.rb"
    cp "$publisher_root/dist/out/$publisher_zip" "$mock_remote/"
    cp "$publisher_root/dist/out/$publisher_pkg" "$mock_remote/"
    cp "$publisher_root/dist/out/SHA256SUMS" "$mock_remote/"
    : > "$mock_log"
}

run_publisher() {
    publisher_answer=$1
    publisher_tag=$2
    printf '%s\n' "$publisher_answer" | env \
        PATH="$mock_bin:$PATH" \
        MOCK_CALL_LOG="$mock_log" \
        MOCK_REMOTE_DIR="$mock_remote" \
        MOCK_COMMIT=0123456789abcdef0123456789abcdef01234567 \
        MOCK_FAIL_PUBLISH="${MOCK_FAIL_PUBLISH:-0}" \
        sh "$publisher_root/dist/publish-release.sh" "$publisher_tag"
}

check_remote_mismatch() {
    mismatch_name=$1
    if mismatch_output=$(run_publisher n v0.3.0-rc2 2>&1); then
        printf '  FAIL publisher rejects remote %s byte mismatch\n' \
            "$mismatch_name" >&2
        failures=$((failures + 1))
    elif printf '%s\n' "$mismatch_output" | grep -qi 'does not match'; then
        printf '  PASS publisher rejects remote %s byte mismatch\n' \
            "$mismatch_name"
    else
        printf '  FAIL publisher %s mismatch diagnostic: %s\n' \
            "$mismatch_name" "$mismatch_output" >&2
        failures=$((failures + 1))
    fi
}

prepare_publisher_case v0.3.0-rc2
printf 'different remote zip\n' > "$mock_remote/$publisher_zip"
check_remote_mismatch ZIP

prepare_publisher_case v0.3.0-rc2
printf 'different remote pkg\n' > "$mock_remote/$publisher_pkg"
check_remote_mismatch PKG

prepare_publisher_case v0.3.0-rc2
printf 'different remote checksums\n' > "$mock_remote/SHA256SUMS"
check_remote_mismatch SHA256SUMS

prepare_publisher_case v0.3.0-rc2
rm -f "$mock_remote/$publisher_pkg"
if missing_output=$(run_publisher n v0.3.0-rc2 2>&1); then
    printf '  FAIL publisher rejects missing remote PKG\n' >&2
    failures=$((failures + 1))
elif printf '%s\n' "$missing_output" | grep -qi 'asset'; then
    printf '  PASS publisher rejects missing remote PKG\n'
else
    printf '  FAIL publisher missing-PKG diagnostic: %s\n' "$missing_output" >&2
    failures=$((failures + 1))
fi

prepare_publisher_case v0.3.0-rc2
if run_publisher y v0.3.0-rc2 >/dev/null 2>&1 &&
   grep -Fq -- 'release edit v0.3.0-rc2 --repo zw3rk/hyper-linux --draft=false --prerelease' \
       "$mock_log"; then
    printf '  PASS publisher marks RC prerelease\n'
else
    printf '  FAIL publisher marks RC prerelease\n' >&2
    failures=$((failures + 1))
fi

prepare_publisher_case v0.3.0
if run_publisher y v0.3.0 >/dev/null 2>&1 &&
   grep -Fq -- 'release edit v0.3.0 --repo zw3rk/hyper-linux --draft=false --prerelease=false' \
       "$mock_log"; then
    printf '  PASS publisher clears prerelease for stable release\n'
else
    printf '  FAIL publisher clears prerelease for stable release\n' >&2
    failures=$((failures + 1))
fi

prepare_publisher_case v0.3.0-rc2
if MOCK_FAIL_PUBLISH=1 run_publisher y v0.3.0-rc2 >/dev/null 2>&1; then
    printf '  FAIL publisher reports failed publication\n' >&2
    failures=$((failures + 1))
fi
if grep -Fq -- 'git revert --no-edit HEAD' "$mock_log" &&
   [ "$(grep -Fc 'git push origin master' "$mock_log")" -eq 2 ]; then
    printf '  PASS failed publication rolls back tap update\n'
else
    printf '  FAIL failed publication rolls back tap update\n' >&2
    failures=$((failures + 1))
fi

if [ "$failures" -ne 0 ]; then
    printf '%d release plumbing test(s) failed\n' "$failures" >&2
    exit 1
fi

printf 'release plumbing: all tests passed\n'
