#!/bin/sh
# Pure helpers shared by release automation and its regression tests.

# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

release_is_uint() {
    case ${1-} in
        ''|*[!0-9]*) return 1 ;;
        0|[1-9]*) return 0 ;;
        *) return 1 ;;
    esac
}

release_parse_version() {
    release_input=${1-}
    release_version=${release_input#v}

    case $release_version in
        *-*)
            release_base=${release_version%%-*}
            release_prerelease=${release_version#*-}
            ;;
        *)
            release_base=$release_version
            release_prerelease=
            ;;
    esac

    release_old_ifs=$IFS
    IFS=.
    # Intentional field splitting of the dotted base version.
    # shellcheck disable=SC2086
    set -- $release_base
    IFS=$release_old_ifs
    [ "$#" -eq 3 ] || return 1
    release_is_uint "$1" || return 1
    release_is_uint "$2" || return 1
    release_is_uint "$3" || return 1

    RELEASE_MAJOR=$1
    RELEASE_MINOR=$2
    RELEASE_PATCH=$3
    RELEASE_BASE_VERSION=$release_base
    # Public result consumed by release.sh and the unit test after sourcing.
    # shellcheck disable=SC2034
    RELEASE_PRERELEASE=$release_prerelease
    RELEASE_RC_NUMBER=

    if [ -n "$release_prerelease" ]; then
        case $release_prerelease in
            rc*) RELEASE_RC_NUMBER=${release_prerelease#rc} ;;
            *) return 1 ;;
        esac
        release_is_uint "$RELEASE_RC_NUMBER" || return 1
    fi

    return 0
}

release_next_major() {
    printf '%s.0.0\n' "$((RELEASE_MAJOR + 1))"
}

release_next_minor() {
    printf '%s.%s.0\n' "$RELEASE_MAJOR" "$((RELEASE_MINOR + 1))"
}

release_next_patch() {
    printf '%s.%s.%s\n' "$RELEASE_MAJOR" "$RELEASE_MINOR" \
        "$((RELEASE_PATCH + 1))"
}

release_next_rc() {
    [ -n "$RELEASE_RC_NUMBER" ] || return 1
    printf '%s-rc%s\n' "$RELEASE_BASE_VERSION" "$((RELEASE_RC_NUMBER + 1))"
}

release_final_version() {
    printf '%s\n' "$RELEASE_BASE_VERSION"
}

release_is_prerelease() {
    [ -n "${RELEASE_PRERELEASE:-}" ]
}

release_artifact_path() {
    release_out_dir=$1
    release_tag=$2
    release_extension=$3

    case $release_tag in
        v*) ;;
        *) release_tag=v$release_tag ;;
    esac
    release_parse_version "$release_tag" || return 1
    case $release_extension in
        zip|pkg) ;;
        *) return 1 ;;
    esac

    printf '%s/hl-%s.%s\n' "$release_out_dir" "$release_tag" \
        "$release_extension"
}
