#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica Unipessoal Lda
#
# Decide from the Tester's summary whether a run matched its expected
# results (conf/trc.xml). run.sh exits 0 whenever the run completes,
# whatever the tests did, so this is what turns a run into a verdict:
# every iteration must have passed or failed as expected - none
# unexpected, aborted, new, or skipped unexpectedly - and at least one
# must have run.
#
# Usage: scripts/check-run.sh <file with the output of run.sh>

set -euo pipefail

out="${1:?usage: $0 <run.sh output>}"

count() {
    # The summary line "  <label>   <n>"; the last run in the file wins.
    { grep -E "^[[:space:]]*$1[[:space:]]+[0-9]+[[:space:]]*\$" "${out}" ||
          true ; } | tail -1 | awk '{ print $NF }'
}

total="$(count 'Run \(total\)')"
if [ -z "${total}" ] ; then
    echo "check-run: no Tester summary in ${out} (the run did not finish)" >&2
    exit 1
fi

bad=0
for label in 'Passed unexpectedly' 'Failed unexpectedly' \
             'Aborted \(no useful result\)' \
             'New \(expected result is not known\)' 'Skipped unexpectedly'
do
    n="$(count "${label}")"
    if [ "${n:-0}" != "0" ] ; then
        echo "check-run: ${label//\\/}: ${n}" >&2
        bad=1
    fi
done

if [ "${total}" = "0" ] ; then
    echo "check-run: no iteration ran" >&2
    bad=1
fi

passed="$(count 'Passed, as expected')"
failed="$(count 'Failed, as expected')"
echo "check-run: ${total} iterations: ${passed:-0} passed and" \
     "${failed:-0} failed as expected"
if [ "${bad}" -ne 0 ] ; then
    echo "check-run: FAILED - the run does not match conf/trc.xml" >&2
    exit 1
fi
echo "check-run: ok"
