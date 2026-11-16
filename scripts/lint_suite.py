# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Interpretica Unipessoal Lda
"""Consistency checks of the suite's own description of itself.

A TE suite says the same thing in several places - the C files, the
meson lists, the package.xml runs and their arguments, the expected
results in conf/trc.xml, the README - and nothing but a full run notices
when they drift apart, often only as a silently wrong expectation. This
checks, without building anything:

 - every test C file is built by meson and run by package.xml, and every
   run has its C file;
 - every run takes the suite's backend argument, with known backends
   only, and every argument a run gives is read by the test;
 - every expected result in conf/trc.xml names an existing test, the
   arguments and values that test is run with, and a verdict the test's
   source can actually produce;
 - every test is described in the README.

Usage: python3 scripts/lint_suite.py   (from the suite's top directory)
"""

import os
import re
import sys
import xml.etree.ElementTree as ET

SUITE = 'tsf-sql-ts'
GROUP = 'sql'
BACKENDS = {'sqlite', 'postgres', 'mariadb'}
# Scripts that are not tests: run once per group, no arguments.
SERVICE = {'prologue', 'epilogue'}
# Tests whose expected verdict is emitted by the library under test (the
# tapi_sql assertions), not by a TEST_VERDICT of their own: for these
# the verdict in conf/trc.xml is checked against this table instead,
# keyed by the argument that selects the property.
LIBRARY_VERDICTS = {
    'assert_violation': ('property', {
        'row_count': 'unexpected number of rows in t_av',
        'pk_unique': 'key (id) of t_av is not unique',
        'referential':
            't_av_child (parent_id) refers to rows missing from t_av',
        'not_null': 'NULL in (name) of t_av',
        'schema': 'schema of t_av is not compatible',
        'equal': 'the query result differs from what is expected',
        'number': 'a number is outside its tolerance',
        'atomic': 'the failing transaction did not fail',
    }),
}

errors = []


def error(msg):
    errors.append(msg)


def c_literals(text):
    """The C string literals of text, adjacent ones joined, unescaped."""
    out = []
    for m in re.finditer(r'(?:"(?:[^"\\\n]|\\.)*"\s*)+', text):
        parts = re.findall(r'"((?:[^"\\\n]|\\.)*)"', m.group(0))
        joined = ''.join(parts)
        out.append(bytes(joined, 'utf-8').decode('unicode_escape'))
    return out


def verdict_patterns(source):
    """Regexes for the verdicts a test source can produce.

    A verdict comes from TEST_VERDICT/TEST_FAIL/RING_VERDICT/TEST_SKIP
    with a format string; a conversion matches any text.
    """
    pats = []
    for m in re.finditer(r'\b(?:TEST_VERDICT|TEST_FAIL|RING_VERDICT|'
                         r'ERROR_VERDICT|WARN_VERDICT|TEST_SKIP)\s*\(',
                         source):
        lits = c_literals(source[m.end():m.end() + 2000])
        if not lits:
            continue
        fmt = lits[0]
        rx = ''
        for piece in re.split(r'(%[-+ #0]*\d*(?:\.\d+)?[hlzjt]*[a-zA-Z%])',
                              fmt):
            if re.fullmatch(r'%[-+ #0]*\d*(?:\.\d+)?[hlzjt]*[a-zA-Z]', piece):
                rx += '.*'
            elif piece == '%%':
                rx += '%'
            else:
                rx += re.escape(piece)
        pats.append(re.compile(rx + r'\Z', re.S))
    return pats


def main():
    group_dir = os.path.join(SUITE, GROUP)

    # --- C files, meson, package.xml ------------------------------------
    c_files = {f[:-2] for f in os.listdir(group_dir) if f.endswith('.c')}

    mb = open(os.path.join(group_dir, 'meson.build')).read()
    m = re.search(r'tests\s*=\s*\[(.*?)\]', mb, re.S)
    meson = set(re.findall(r"'([^']+)'", m.group(1))) if m else set()
    if not m:
        error(f'{group_dir}/meson.build: no tests = [...] list')
    for t in sorted(c_files - meson):
        error(f'{GROUP}/{t}.c is not in meson.build')
    for t in sorted(meson - c_files):
        error(f'meson.build lists {t}, which has no C file')

    pkg = ET.parse(os.path.join(group_dir, 'package.xml')).getroot()
    session = pkg.find('session')
    service = {s.get('name') for tag in ('prologue', 'epilogue')
               for s in session.iter(tag) for s in s.iter('script')}
    for s in sorted(SERVICE - service):
        error(f'{GROUP}/package.xml has no {s}')

    runs = {}
    for run in session.findall('run'):
        script = run.find('script')
        if script is None:
            error(f'{GROUP}/package.xml: a run without a script')
            continue
        name = script.get('name')
        # A test may be run by several blocks (one per engine family,
        # with the argument values that make sense there): the values
        # it is run with are the union.
        args = runs.setdefault(name, {})
        for arg in run.findall('arg'):
            values = [v.text for v in arg.findall('value')]
            if not values:
                error(f'{name}: argument {arg.get("name")} has no values')
            if len(values) != len(set(values)):
                error(f'{name}: argument {arg.get("name")} repeats a value')
            known = args.setdefault(arg.get('name'), [])
            known.extend(v for v in values if v not in known)
        if script.find('objective') is None:
            error(f'{name}: no objective in package.xml')

    tests = c_files - SERVICE
    for t in sorted(tests - set(runs)):
        error(f'{GROUP}/{t}.c is not run by package.xml')
    for t in sorted(set(runs) - tests):
        error(f'package.xml runs {t}, which has no C file')

    # --- arguments --------------------------------------------------------
    for name, args in sorted(runs.items()):
        src = open(os.path.join(group_dir, name + '.c')).read()
        if 'backend' not in args:
            error(f'{name}: no backend argument')
        elif set(args['backend']) - BACKENDS:
            error(f'{name}: unknown backend '
                  f'{sorted(set(args["backend"]) - BACKENDS)}')
        for arg in args:
            if not re.search(r'TEST_GET_\w+_PARAM\(\s*%s\s*\)' % arg, src):
                error(f'{name}: argument {arg} is never read by the test')

    # --- expected results -------------------------------------------------
    trc = ET.parse(os.path.join('conf', 'trc.xml')).getroot()
    top = trc.find('test')
    if top is None or top.get('name') != SUITE:
        error(f'conf/trc.xml: the top package is not {SUITE}')
    else:
        group = None
        for it in top.findall('iter'):
            for t in it.findall('test'):
                if t.get('name') == GROUP:
                    group = t
        if group is None:
            error(f'conf/trc.xml: no {GROUP} package')
        else:
            for it in group.findall('iter'):
                for t in it.findall('test'):
                    check_trc_test(t, runs, group_dir)

    # --- README -----------------------------------------------------------
    readme = open('README.md').read()
    for name in sorted(runs):
        if not re.search(r'^\| `%s` \|' % re.escape(name), readme, re.M):
            error(f'README.md: no row for {name} in the test table')

    for e in errors:
        print(f'lint_suite: {e}', file=sys.stderr)
    if errors:
        print(f'lint_suite: FAILED ({len(errors)} problems)', file=sys.stderr)
        return 1
    print(f'lint_suite: ok ({len(runs)} tests)')
    return 0


def check_trc_test(t, runs, group_dir):
    name = t.get('name')
    if name not in runs:
        error(f'conf/trc.xml: {name} is not a test of the suite')
        return
    declared = runs[name]
    pats = verdict_patterns(open(os.path.join(group_dir,
                                              name + '.c')).read())
    for it in t.findall('iter'):
        args = {a.get('name'): a.text for a in it.findall('arg')}
        where = f'conf/trc.xml: {name} {args}'
        if set(args) != set(declared):
            error(f'{where}: arguments are {sorted(args)}, the test takes '
                  f'{sorted(declared)}')
        for a, v in args.items():
            if a in declared and v not in declared[a]:
                error(f'{where}: {a}={v} is not a value it is run with')
        if it.find('notes') is None or not (it.find('notes').text or ''):
            error(f'{where}: an expected failure without notes')
        for res in it.iter('result'):
            for v in res.findall('verdict'):
                text = v.text or ''
                if name in LIBRARY_VERDICTS:
                    key, table = LIBRARY_VERDICTS[name]
                    if table.get(args.get(key)) != text:
                        error(f'{where}: the verdict "{text}" is not the '
                              f'one the assertion of {args.get(key)} emits')
                elif not any(p.match(text) for p in pats):
                    error(f'{where}: the test cannot produce the verdict '
                          f'"{text}"')


if __name__ == '__main__':
    sys.exit(main())
