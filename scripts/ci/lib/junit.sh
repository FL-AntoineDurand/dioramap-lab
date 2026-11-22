#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# JUnit XML emitter — produces a single <testsuites> file from a stream
# of test records.
#
# Bash-only. No external deps. Escapes XML special chars on the way in.
#
# Usage shape:
#   . scripts/ci/lib/junit.sh
#   junit_begin <suite-name>
#   junit_record <name> <pass|fail|skip> <duration_seconds> <stdout-file> [failure-message]
#   junit_record ...
#   junit_end <out.xml>

set -uo pipefail

# Internal state — guarded against re-source by a generation counter so
# scripts can be re-sourced inside a single CI run without leaking records.
declare -a __JUNIT_RECORDS=()
__JUNIT_SUITE=""

# XML-escape stdin → stdout. Handles &, <, >, ", '.
__junit_escape() {
    sed -e 's/&/\&amp;/g' \
        -e 's/</\&lt;/g' \
        -e 's/>/\&gt;/g' \
        -e 's/"/\&quot;/g' \
        -e "s/'/\&apos;/g"
}

# Strip ANSI escape codes + non-printable chars that would corrupt CDATA.
# Two passes: tr to drop the dangerous control-char range, then sed to
# escape the literal "]]>" CDATA terminator and strip ANSI CSI sequences.
__junit_clean() {
    LC_ALL=C tr -d '\000-\010\013\014\016-\037' \
      | sed -e 's/\x1b\[[0-9;]*[a-zA-Z]//g' \
            -e 's/]]>/]]]]><![CDATA[>/g'
}

junit_begin() {
    __JUNIT_SUITE="$1"
    __JUNIT_RECORDS=()
}

# Arguments:
#   $1 testcase name        (no spaces ideal; we'll just escape anyway)
#   $2 pass | fail | skip
#   $3 duration in seconds (decimal allowed)
#   $4 path to stdout file  (may be empty / non-existent → empty system-out)
#   $5 failure message      (only used when $2 == fail; optional)
junit_record() {
    local name="$1" status="$2" duration="$3" stdout_file="${4:-}" fmsg="${5:-}"
    # Append a pipe-delimited line; XML rendering deferred to junit_end so
    # we can compute aggregate counters in one pass.
    __JUNIT_RECORDS+=("${name}|${status}|${duration}|${stdout_file}|${fmsg}")
}

junit_end() {
    local out="$1"
    local n=${#__JUNIT_RECORDS[@]}
    local failures=0 errors=0 skipped=0
    local total_time=0

    # First pass: counts.
    for line in "${__JUNIT_RECORDS[@]}"; do
        local IFS='|'; read -r _name status duration _stdout _fmsg <<<"$line"
        case "$status" in
            fail) failures=$((failures + 1)) ;;
            skip) skipped=$((skipped + 1)) ;;
        esac
        # bash can't add decimals; use awk for cumulative time.
        total_time=$(awk "BEGIN{print $total_time + $duration}")
    done

    local suite_esc
    suite_esc="$(printf '%s' "$__JUNIT_SUITE" | __junit_escape)"

    {
        echo '<?xml version="1.0" encoding="UTF-8"?>'
        printf '<testsuites name="%s" tests="%d" failures="%d" errors="%d" skipped="%d" time="%s">\n' \
            "$suite_esc" "$n" "$failures" "$errors" "$skipped" "$total_time"
        printf '  <testsuite name="%s" tests="%d" failures="%d" errors="%d" skipped="%d" time="%s">\n' \
            "$suite_esc" "$n" "$failures" "$errors" "$skipped" "$total_time"

        for line in "${__JUNIT_RECORDS[@]}"; do
            local IFS='|'; read -r name status duration stdout_file fmsg <<<"$line"
            local name_esc fmsg_esc
            name_esc="$(printf '%s' "$name" | __junit_escape)"
            fmsg_esc="$(printf '%s' "$fmsg" | __junit_escape)"

            printf '    <testcase name="%s" classname="%s" time="%s">\n' \
                "$name_esc" "$suite_esc" "$duration"

            case "$status" in
                fail)
                    printf '      <failure message="%s" type="AssertionError"><![CDATA[\n' "$fmsg_esc"
                    if [[ -n "$stdout_file" && -f "$stdout_file" ]]; then
                        __junit_clean < "$stdout_file"
                    fi
                    printf '\n      ]]></failure>\n'
                    ;;
                skip)
                    printf '      <skipped/>\n'
                    ;;
            esac
            if [[ "$status" != "fail" && -n "$stdout_file" && -f "$stdout_file" ]]; then
                printf '      <system-out><![CDATA[\n'
                __junit_clean < "$stdout_file"
                printf '\n      ]]></system-out>\n'
            fi
            printf '    </testcase>\n'
        done

        echo '  </testsuite>'
        echo '</testsuites>'
    } > "$out"
}
