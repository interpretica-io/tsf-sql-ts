#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica Unipessoal Lda
#
# Static checks of the suite, the same locally and in CI:
#
#  - layout: no tabs, no trailing blanks, a final newline, no CR, and at
#    most 80 columns in C, headers and Python;
#  - every C, Python and shell source carries the SPDX licence line and
#    the copyright line;
#  - XML is well-formed (package.xml files, conf/*.xml);
#  - scripts/lint_suite.py: tests, meson, package.xml, the expected
#    results in conf/trc.xml and the README agree with each other;
#  - shellcheck over the scripts, actionlint over the workflows (when
#    actionlint is present).
#
# The suite's C sources are compiled with -Werror by the suite build;
# they include the Test Environment's headers, so they are not compiled
# here.
#
# Usage: scripts/lint.sh     (from anywhere; non-zero on any finding)

set -uo pipefail

cd "$(dirname "$0")/.." || exit 1

failed=0
fail() {
    echo "lint: $*" >&2
    failed=1
}

# The files git tracks or would track (not ignored) matching patterns.
files() {
    git ls-files --cached --others --exclude-standard "$@" | sort -u
}

# Arrays filled with read loops: bash 3.2 (macOS) has no mapfile.
code_files=() ; src_files=() ; sh_files=() ; xml_files=() ; text_files=()
while IFS= read -r f ; do code_files+=("$f") ; done < \
    <(files '*.c' '*.h' '*.py')
while IFS= read -r f ; do src_files+=("$f") ; done < \
    <(files '*.c' '*.h' '*.py' '*.sh')
while IFS= read -r f ; do sh_files+=("$f") ; done < <(files '*.sh')
while IFS= read -r f ; do xml_files+=("$f") ; done < <(files '*.xml')
while IFS= read -r f ; do text_files+=("$f") ; done < \
    <(files '*.c' '*.h' '*.py' '*.sh' '*.xml' '*.yml' 'meson.build' \
            '*.md' '*.dox' 'conf/*' 'scripts/docker/build/Dockerfile' \
            '.gitignore' 'LICENSE')

# --- layout -------------------------------------------------------------
for f in ${text_files[@]+"${text_files[@]}"} ; do
    [ -f "$f" ] || continue
    if grep -n $'\t' "$f" >/dev/null ; then
        grep -n $'\t' "$f" | head -3 | sed "s|^|$f:|" >&2
        fail "$f: tab character (indent with spaces)"
    fi
    if grep -n '[[:space:]]$' "$f" >/dev/null ; then
        grep -n '[[:space:]]$' "$f" | head -3 | sed "s|^|$f:|" >&2
        fail "$f: trailing blanks"
    fi
    if grep -q $'\r' "$f" ; then
        fail "$f: CR line endings"
    fi
    if [ -s "$f" ] && [ "$(tail -c 1 "$f" | od -An -c | tr -d ' ')" != '\n' ]
    then
        fail "$f: no newline at the end of the file"
    fi
done

for f in ${code_files[@]+"${code_files[@]}"} ; do
    awk -v f="$f" 'length > 80 {
        printf "%s:%d: %d columns\n", f, FNR, length; bad = 1 }
        END { exit bad }' "$f" >&2 || fail "$f: lines longer than 80 columns"
done

# --- licence and copyright ----------------------------------------------
for f in ${src_files[@]+"${src_files[@]}"} ; do
    head -10 "$f" | grep -q 'SPDX-License-Identifier: MIT' ||
        fail "$f: no 'SPDX-License-Identifier: MIT' in the first lines"
    head -10 "$f" | grep -q 'Copyright (C) 20[0-9-]* Interpretica' ||
        fail "$f: no Interpretica copyright line in the first lines"
done

# --- XML and the suite's self-description ---------------------------------
if command -v xmllint >/dev/null ; then
    for f in ${xml_files[@]+"${xml_files[@]}"} ; do
        xmllint --noout "$f" || fail "$f: malformed XML"
    done
else
    fail "xmllint is not installed"
fi

python3 scripts/lint_suite.py || fail "the suite's descriptions disagree"

# --- shellcheck ---------------------------------------------------------
# Warnings and errors fail; the info and style levels (mostly unquoted
# lists that are meant to split, such as docker run options built up in
# a variable) are left to review.
if command -v shellcheck >/dev/null ; then
    shellcheck -x -S warning ${sh_files[@]+"${sh_files[@]}"} ||
        fail "shellcheck reported findings"
else
    fail "shellcheck is not installed"
fi

# --- actionlint ---------------------------------------------------------
if [ ! -d .github/workflows ] ; then
    :
elif command -v actionlint >/dev/null ; then
    actionlint || fail "actionlint reported findings"
else
    echo "lint: actionlint is not installed; workflows not checked" >&2
fi

if [ "${failed}" -ne 0 ] ; then
    echo "lint: FAILED" >&2
    exit 1
fi
echo "lint: ok"
