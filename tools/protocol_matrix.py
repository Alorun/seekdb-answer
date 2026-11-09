#!/usr/bin/env python3
"""Protocol / stdin framing edge-case matrix for the MiniVectorDB binaries.

The official test driver is not public, so this script empirically maps what
the current binaries do for the protocol knobs that the platform may or may
not use:

  * semicolon framing (default) vs ``--statement-mode line``
  * quiet DML (default) vs ``--dml-ack``
  * default ``ready`` marker vs ``--ready-marker TEXT``
  * multi-line statements, several statements per line, ``--`` / ``#`` /
    ``/* */`` comments, string escapes and semicolons inside strings
  * EOF without ``;``, EXIT/QUIT, SIGTERM (clean shutdown), SIGKILL (recovery)
  * stdout purity, ready-before-results, flush timing, error line format,
    state across restart

It never rebuilds or writes core files.  Every process gets a fresh
``--data-dir`` under a temp directory.

Typical use::

    python3 tools/protocol_matrix.py \
        --bin ./build/minivectordb --bin ./build-lead/minivectordb

Exit status is 1 when at least one probe FAILs (QUIRK/SKIP do not fail).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import signal
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mvdb_harness import (Session, new_data_dir, remove_data_dir,  # noqa: E402
                          run_batch, strip_ready)

PASS, FAIL, QUIRK, SKIP = "PASS", "FAIL", "QUIRK", "SKIP"
CELL = {PASS: "P", FAIL: "F", QUIRK: "Q", SKIP: "."}

READY_TIMEOUT = 2.0
RESULT_TIMEOUT = 1.4

ERROR_RE = re.compile(r"^ERROR ([0-9]{4}): .+$")


# --------------------------------------------------------------------------
# configurations: all 2x2x2 combinations named in the task
# --------------------------------------------------------------------------
def make_configs():
    configs = []
    for line in (False, True):
        for ack in (False, True):
            for custom in (False, True):
                args = []
                if line:
                    args += ["--statement-mode", "line"]
                if ack:
                    args += ["--dml-ack"]
                marker = "READY-CUSTOM" if custom else "ready"
                if custom:
                    args += ["--ready-marker", marker]
                labels = ["ln" if line else "sc"]
                if ack:
                    labels.append("A")
                if custom:
                    labels.append("M")
                configs.append({
                    "key": "+".join(labels),
                    "args": args,
                    "line": line,
                    "ack": ack,
                    "custom": custom,
                    "marker": marker,
                })
    return configs


class Ctx:
    """One binary + one configuration; owns temp dirs/sessions."""

    def __init__(self, bin_path, cfg):
        self.bin = bin_path
        self.cfg = cfg
        self.args = cfg["args"]
        self.marker = cfg["marker"]
        self.line = cfg["line"]
        self.ack = cfg["ack"]
        self._dirs = []
        self._sessions = []

    def dir(self):
        d = new_data_dir("mvdb-proto-")
        self._dirs.append(d)
        return d

    def session(self, data_dir=None, extra_args=()):
        s = Session(self.bin, data_dir or self.dir(),
                    args=list(self.args) + list(extra_args))
        self._sessions.append(s)
        return s

    def batch(self, script, data_dir=None, extra_args=(), timeout=20.0):
        return run_batch(self.bin, data_dir or self.dir(), script,
                         args=list(self.args) + list(extra_args),
                         timeout=timeout)

    def ack_lines(self, affected):
        return ["OK %d" % n for n in affected]

    def cleanup(self):
        for s in self._sessions:
            try:
                s.close()
            except Exception:
                pass
        for d in self._dirs:
            remove_data_dir(d)


def fail_detail(expected, observed):
    return "expected %r; observed %r" % (expected, observed)


def wait_and_lines(s, total, timeout=RESULT_TIMEOUT):
    s.wait_lines(total, timeout)
    s.pump(0.05)
    return s.out_lines()


def expect_full(ctx, s, expected, timeout=RESULT_TIMEOUT):
    """Compare the full stdout transcript with [marker] + expected."""
    wanted = [ctx.marker] + expected
    got = wait_and_lines(s, len(wanted), timeout)
    if got == wanted:
        return PASS, ""
    return FAIL, fail_detail(wanted, got)


def expect_prefix(ctx, s, expected_tail, timeout=RESULT_TIMEOUT):
    """Compare everything after the marker (allows extra trailing lines)."""
    wanted = [ctx.marker] + expected_tail
    got = wait_and_lines(s, len(wanted), timeout)
    if got[:len(wanted)] == wanted:
        return PASS, ""
    return FAIL, fail_detail(wanted, got[:len(wanted)])


# --------------------------------------------------------------------------
# probes that run for every configuration
# --------------------------------------------------------------------------
def probe_ready_and_purity(ctx):
    s = ctx.session()
    s.wait_lines(1, READY_TIMEOUT)
    s.pump(0.05)
    lines = s.out_lines()
    if not lines:
        return FAIL, "no stdout within %.1fs (startup blocked?)" % READY_TIMEOUT
    if lines[0] != ctx.marker:
        return FAIL, fail_detail(ctx.marker, lines[0])
    if any("minivectordb:" in ln for ln in lines):
        return FAIL, "startup banner leaked to stdout: %r" % lines
    err = s.err_text()
    if "minivectordb: data_dir=" not in err:
        return FAIL, "startup banner missing from stderr: %r" % err
    return PASS, ""


def probe_basic_select_flush(ctx):
    s = ctx.session()
    s.send("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8));\n")
    s.send("INSERT INTO t VALUES (1,'a'),(2,'b');\n")
    s.send("SELECT id FROM t ORDER BY id;\n")
    expected = ([ctx.marker] + (ctx.ack_lines([0, 2]) if ctx.ack else [])
                + ["id", "1", "2"])
    got = wait_and_lines(s, len(expected))
    # this read happens while stdin is still open -> proves per-result flush
    if got != expected:
        return FAIL, fail_detail(expected, got)
    return PASS, "result flushed before stdin was closed"


def probe_dml_ack_policy(ctx):
    s = ctx.session()
    s.send("CREATE TABLE t (id BIGINT PRIMARY KEY);\n")
    s.send("INSERT INTO t VALUES (1),(2);\n")
    s.send("BEGIN;\n")
    s.send("COMMIT;\n")
    s.send("SELECT id FROM t ORDER BY id;\n")
    acks = ctx.ack_lines([0, 2, 0, 0]) if ctx.ack else []
    expected = [ctx.marker] + acks + ["id", "1", "2"]
    got = wait_and_lines(s, len(expected))
    if got != expected:
        return FAIL, fail_detail(expected, got)
    if ctx.ack:
        return PASS, "quirk: DDL/BEGIN/COMMIT also print 'OK 0'"
    return PASS, ""


def probe_error_format(ctx):
    s = ctx.session()
    s.send("SELECT id FROM missing;\n")
    got = wait_and_lines(s, 2)
    if len(got) < 2:
        return FAIL, "no error line within timeout: %r" % got
    line = got[1]
    if not ERROR_RE.match(line) or not line.startswith("ERROR 1146:"):
        return FAIL, fail_detail("ERROR 1146: <message>", line)
    if got[0] != ctx.marker:
        return FAIL, fail_detail(ctx.marker, got[0])
    return PASS, "errors are written to stdout (not stderr)"


def probe_multiline_ddl(ctx):
    s = ctx.session()
    script = (
        "CREATE TABLE t (\n"
        "  id BIGINT PRIMARY KEY,\n"
        "  s VARCHAR(8)\n"
        ");\n"
        "INSERT INTO t\n"
        "  VALUES (1,'x'),(2,'y');\n"
        "SELECT id,s FROM t ORDER BY id;\n"
    )
    s.send(script)
    acks = ctx.ack_lines([0, 2]) if ctx.ack else []
    expected = [ctx.marker] + acks + ["id\ts", "1\tx", "2\ty"]
    got = wait_and_lines(s, len(expected), 1.8)
    if got != expected:
        return FAIL, fail_detail(expected, got)
    return PASS, ""


def probe_multiline_continuation(ctx):
    s = ctx.session()
    script = (
        "CREATE TABLE t (id BIGINT PRIMARY KEY);\n"
        "INSERT INTO t VALUES (1),(2),(3);\n"
        "SELECT id\n"
        "FROM t\n"
        "ORDER BY id DESC;\n"
    )
    s.send(script)
    acks = ctx.ack_lines([0, 3]) if ctx.ack else []
    expected = [ctx.marker] + acks + ["id", "3", "2", "1"]
    got = wait_and_lines(s, len(expected), 1.8)
    if got != expected:
        extra = ""
        if ctx.line:
            extra = ("; line mode executes the statement as soon as it parses "
                     "at EOL, so continuation clauses on later lines break")
        return FAIL, fail_detail(expected, got) + extra
    return PASS, ""


def probe_multi_per_line(ctx):
    s = ctx.session()
    s.send("CREATE TABLE t (id BIGINT PRIMARY KEY);"
           "INSERT INTO t VALUES (1);"
           "INSERT INTO t VALUES (2);"
           "SELECT id FROM t ORDER BY id;\n")
    acks = ctx.ack_lines([0, 1, 1]) if ctx.ack else []
    expected = [ctx.marker] + acks + ["id", "1", "2"]
    got = wait_and_lines(s, len(expected), 1.8)
    if got != expected:
        return FAIL, fail_detail(expected, got)
    return PASS, ""


def probe_comments(ctx):
    s = ctx.session()
    script = (
        "-- leading comment ; with semicolon\n"
        "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8));"
        " # trailing hash ; comment\n"
        "/* block ; comment */\n"
        "INSERT INTO t VALUES (1,'a'),(2,'b');"
        " /* inline ; comment */\n"
        "-- trailing line comment ;\n"
        "SELECT id,s FROM t ORDER BY id; /* tail */\n"
    )
    s.send(script)
    acks = ctx.ack_lines([0, 2]) if ctx.ack else []
    expected = [ctx.marker] + acks + ["id\ts", "1\ta", "2\tb"]
    got = wait_and_lines(s, len(expected), 1.8)
    if got != expected:
        return FAIL, fail_detail(expected, got)
    return PASS, ""


def probe_string_escapes(ctx):
    s = ctx.session()
    script = (
        "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(32));\n"
        "INSERT INTO t VALUES (1,'a;b');\n"
        "INSERT INTO t VALUES (2,'it''s');\n"
        "INSERT INTO t VALUES (3,'back\\\\slash');\n"
        "SELECT id,s FROM t ORDER BY id;\n"
    )
    s.send(script)
    acks = ctx.ack_lines([0, 1, 1, 1]) if ctx.ack else []
    expected = ([ctx.marker] + acks
                + ["id\ts", "1\ta;b", "2\tit's", "3\tback\\slash"])
    got = wait_and_lines(s, len(expected), 1.8)
    if got != expected:
        return FAIL, fail_detail(expected, got)
    return PASS, ""


def probe_eof_no_semicolon(ctx):
    d = ctx.dir()
    rc, out, err = ctx.batch(
        "CREATE TABLE t (id BIGINT PRIMARY KEY);\n"
        "INSERT INTO t VALUES (1)\n", data_dir=d)
    if rc != 0:
        return FAIL, "first process rc=%r stderr=%r" % (rc, err.decode("utf-8", "replace"))
    rc, out, err = ctx.batch("SELECT id FROM t;\n", data_dir=d)
    got = strip_ready(out.decode("utf-8", "backslashreplace"), ctx.marker)
    # the second process has the same marker config; strip_ready handles it
    expected = ["id", "1"]
    if got != expected:
        return FAIL, fail_detail(expected, got)
    return PASS, "statement without ';' executed at EOF"


def probe_restart_persistence(ctx):
    d = ctx.dir()
    ctx.batch("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8));\n"
              "INSERT INTO t VALUES (7,'persist');\n", data_dir=d)
    s = ctx.session(data_dir=d)
    s.send("SELECT id,s FROM t;\n")
    expected = [ctx.marker, "id\ts", "7\tpersist"]
    got = wait_and_lines(s, len(expected))
    if got != expected:
        return FAIL, fail_detail(expected, got)
    return PASS, "state survives restart"


def probe_exit_quit(ctx):
    s = ctx.session()
    s.wait_lines(1, READY_TIMEOUT)
    s.send("EXIT;\n")
    code = s.wait(2.0)
    if code is None:
        s.kill9()
        return FAIL, "process still running 2s after 'EXIT;'"
    if code != 0:
        return FAIL, "EXIT exit code %r (expected 0)" % code
    s2 = ctx.session()
    s2.wait_lines(1, READY_TIMEOUT)
    s2.send("QUIT;\n")
    code2 = s2.wait(2.0)
    if code2 is None:
        s2.kill9()
        return FAIL, "process still running 2s after 'QUIT;'"
    if code2 != 0:
        return FAIL, "QUIT exit code %r (expected 0)" % code2
    return PASS, ""


def probe_no_newline_statement(ctx):
    if ctx.line:
        return SKIP, "line mode is line-oriented by definition"
    s = ctx.session()
    s.send("CREATE TABLE t (id BIGINT PRIMARY KEY);\n")
    s.send("INSERT INTO t VALUES (1),(2);\n")
    s.wait_lines(2 if ctx.ack else 2, 1.0)
    s.pump(0.1)
    s.take_out()
    s.send("SELECT id FROM t ORDER BY id;")  # deliberately no '\n'
    if not s.wait_contains(b"id\n1\n2\n", RESULT_TIMEOUT):
        return FAIL, ("no result for a complete ';' statement without a "
                      "trailing newline; observed %r" % s.out_text())
    s.take_out()
    s.send("INSERT INTO t VALUES (3);SELECT id FROM t ORDER BY id;")
    if not s.wait_contains(b"1\n2\n3\n", RESULT_TIMEOUT):
        return FAIL, ("second/new statements without newline were not "
                      "processed; observed %r" % s.out_text())
    return PASS, "complete ';' statements do not need a trailing newline"


def probe_split_chunk(ctx):
    if ctx.line:
        return SKIP, "chunk splitting is only meaningful in semicolon mode"
    s = ctx.session()
    s.send("CREATE TABLE t (id BIGINT PRIMARY KEY);\n")
    s.send("INSERT INTO t VALUES (1),(2);\n")
    s.wait_lines(1, 1.0)
    s.pump(0.1)
    s.take_out()
    s.send("SELECT i")  # split inside a token
    s.pump(0.25)
    if b"id" in s.out_bytes():
        return FAIL, "partial statement produced output before it was complete"
    s.send("d FROM t;")
    if not s.wait_contains(b"id\n1\n2\n", RESULT_TIMEOUT):
        return FAIL, ("statement split across two writes was not assembled; "
                      "observed %r" % s.out_text())
    return PASS, "buffer survives arbitrary chunk boundaries"


def probe_parse_error_isolation(ctx):
    d = ctx.dir()
    script = (
        "CREATE TABLE t (id BIGINT PRIMARY KEY);\n"
        "CREATE TABLE bad (s VARCHAR(2) PRIMARY KEY);\n"
        "INSERT INTO t VALUES (1);\n"
        "SELECT id FROM t;\n"
    )
    rc, out, err = ctx.batch(script, data_dir=d)
    got = strip_ready(out.decode("utf-8", "backslashreplace"), ctx.marker)
    # CREATE t emits OK 0 when --dml-ack is on, which shifts the error line.
    expected_prefix = (["OK 0"] if ctx.ack else []) + ["ERROR 1235:"]
    if len(got) < len(expected_prefix) + 1:
        return FAIL, "expected a 1235 error for the VARCHAR primary key: %r" % got
    for i, want in enumerate(expected_prefix):
        if want.endswith(":"):
            if not got[i].startswith(want):
                return FAIL, "expected %r at line %d: %r" % (want, i, got)
        elif got[i] != want:
            return FAIL, "expected %r at line %d: %r" % (want, i, got)
    tail = got[len(expected_prefix):]
    expected_tail = (["OK 1"] if ctx.ack else []) + ["id", "1"]
    if tail != expected_tail:
        return FAIL, ("statements after a parse error were dropped; observed %r"
                      % got)
    return PASS, "following statements still execute"


CORE_PROBES = [
    ("ready_purity", probe_ready_and_purity),
    ("basic_flush", probe_basic_select_flush),
    ("dml_ack", probe_dml_ack_policy),
    ("error_format", probe_error_format),
    ("multiline_ddl", probe_multiline_ddl),
    ("multiline_cont", probe_multiline_continuation),
    ("multi_per_line", probe_multi_per_line),
    ("comments", probe_comments),
    ("string_escapes", probe_string_escapes),
    ("eof_no_semicolon", probe_eof_no_semicolon),
    ("restart_persist", probe_restart_persistence),
    ("exit_quit", probe_exit_quit),
    ("no_newline_stmt", probe_no_newline_statement),
    ("split_chunk", probe_split_chunk),
    ("parse_error_isol", probe_parse_error_isolation),
]


# --------------------------------------------------------------------------
# special probes: run once per binary, not per configuration
# --------------------------------------------------------------------------
def special_probes():
    return [
        ("empty_marker", sp_empty_marker),
        ("exit_no_newline", sp_exit_no_newline),
        ("exit_no_terminator_eof", sp_exit_no_terminator_eof),
        ("trailing_bad_string", sp_trailing_bad_string),
        ("sigterm_rollback", sp_sigterm_rollback),
        ("sigterm_pending", sp_sigterm_pending),
        ("sigkill_uncommitted", sp_sigkill_uncommitted),
        ("sigkill_committed", sp_sigkill_committed),
        ("control_chars_tsv", sp_control_chars),
        ("dash_no_space", sp_dash_no_space),
        ("long_statement", sp_long_statement),
        ("unknown_arg", sp_unknown_arg),
        ("bad_data_dir", sp_bad_data_dir),
        ("no_final_newline", sp_no_final_newline),
        ("split_leading_kw", sp_split_leading_keyword),
    ]


def sp_empty_marker(bin_path):
    d = new_data_dir()
    try:
        s = Session(bin_path, d, args=["--ready-marker", ""])
        s.pump(0.3)
        s.send("CREATE TABLE t (id BIGINT PRIMARY KEY);\n")
        s.send("INSERT INTO t VALUES (1);\n")
        s.send("SELECT id FROM t;\n")
        got = wait_and_lines(s, 2, 1.0)
        if got != ["id", "1"]:
            return FAIL, fail_detail(["id", "1"], got)
        return PASS, "empty --ready-marker suppresses the marker line"
    finally:
        s.close()
        remove_data_dir(d)


def sp_exit_no_newline(bin_path):
    d = new_data_dir()
    s = Session(bin_path, d)
    try:
        s.wait_lines(1, READY_TIMEOUT)
        s.send("EXIT;")  # complete statement, no newline, stdin stays open
        code = s.wait(1.2)
        if code is None:
            return FAIL, ("process did not exit for 'EXIT;' without a trailing "
                          "newline; it waits for more input")
        if code != 0:
            return FAIL, "exit code %r for 'EXIT;'" % code
        return PASS, ""
    finally:
        if s.alive():
            s.kill9()
        s.close()
        remove_data_dir(d)


def sp_exit_no_terminator_eof(bin_path):
    d = new_data_dir()
    s = Session(bin_path, d)
    try:
        s.wait_lines(1, READY_TIMEOUT)
        s.send("EXIT\n")  # no ';' at all
        s.close_stdin()
        code = s.wait(2.0)
        if code is None:
            s.kill9()
            return FAIL, "'EXIT' without ';' did not exit at EOF"
        if code != 0:
            return FAIL, "exit code %r" % code
        return PASS, "executed at EOF"
    finally:
        s.close()
        remove_data_dir(d)


def sp_trailing_bad_string(bin_path):
    d = new_data_dir()
    try:
        run_batch(bin_path, d, "CREATE TABLE t (id BIGINT PRIMARY KEY);\n")
        rc, out, err = run_batch(
            bin_path, d,
            "INSERT INTO t VALUES (1);\n"
            "INSERT INTO t VALUES (2,'x);\n")
        got = strip_ready(out.decode("utf-8", "backslashreplace"))
        if not any(line.startswith("ERROR 1064:") for line in got):
            return FAIL, "missing 1064 for unterminated string: %r" % got
        rc, out, err = run_batch(bin_path, d, "SELECT id FROM t;\n")
        rows = strip_ready(out.decode("utf-8", "backslashreplace"))
        if rows != ["id", "1"]:
            return FAIL, ("prior statement lost when a later unterminated "
                          "string sits in the same input chunk; observed %r"
                          % rows)
        return PASS, "earlier statements survive a trailing lexical error"
    finally:
        remove_data_dir(d)


def _prepare(bin_path, d):
    run_batch(bin_path, d, "CREATE TABLE t (id BIGINT PRIMARY KEY);\n")


def _rows(bin_path, d):
    rc, out, err = run_batch(bin_path, d, "SELECT id FROM t ORDER BY id;\n")
    return strip_ready(out.decode("utf-8", "backslashreplace"))


def sp_sigterm_rollback(bin_path):
    d = new_data_dir()
    s = None
    try:
        _prepare(bin_path, d)
        s = Session(bin_path, d)
        s.wait_lines(1, READY_TIMEOUT)
        s.send("BEGIN;\n")
        s.send("INSERT INTO t VALUES (1);\n")
        s.pump(0.2)
        code = s.terminate(signal.SIGTERM, 3.0)
        if code is None:
            s.kill9()
            return FAIL, "no exit within 3s of SIGTERM"
        rows = _rows(bin_path, d)
        if rows != ["id"]:
            return FAIL, ("active transaction was not rolled back; restart "
                          "shows %r" % rows)
        if code != 0:
            return FAIL, "SIGTERM exit code %r" % code
        return PASS, ""
    finally:
        if s:
            s.close()
        remove_data_dir(d)


def sp_sigterm_pending(bin_path):
    d = new_data_dir()
    s = None
    try:
        _prepare(bin_path, d)
        s = Session(bin_path, d)
        s.wait_lines(1, READY_TIMEOUT)
        # complete autocommit statement, but no ';' and no newline terminator
        s.send("INSERT INTO t VALUES (7)\n")
        s.pump(0.2)
        code = s.terminate(signal.SIGTERM, 3.0)
        if code is None:
            s.kill9()
            return FAIL, "no exit within 3s of SIGTERM"
        rows = _rows(bin_path, d)
        if rows != ["id"]:
            return FAIL, ("pending unterminated statement was EXECUTED during "
                          "SIGTERM shutdown and committed; restart shows %r"
                          % rows)
        return PASS, "pending input discarded on SIGTERM"
    finally:
        if s:
            s.close()
        remove_data_dir(d)


def sp_sigkill_uncommitted(bin_path):
    d = new_data_dir()
    s = None
    try:
        _prepare(bin_path, d)
        s = Session(bin_path, d)
        s.wait_lines(1, READY_TIMEOUT)
        s.send("BEGIN;\n")
        s.send("INSERT INTO t VALUES (2);\n")
        s.pump(0.25)
        s.kill9()
        rows = _rows(bin_path, d)
        if rows != ["id"]:
            return FAIL, ("uncommitted row recovered after SIGKILL: %r" % rows)
        return PASS, "uncommitted tail discarded"
    finally:
        if s:
            s.close()
        remove_data_dir(d)


def sp_sigkill_committed(bin_path):
    d = new_data_dir()
    s = None
    try:
        _prepare(bin_path, d)
        s = Session(bin_path, d, args=["--dml-ack"])
        s.wait_lines(1, READY_TIMEOUT)
        s.send("INSERT INTO t VALUES (3);\n")
        if not s.wait_contains(b"OK 1", 1.5):
            return FAIL, "no DML ack before SIGKILL: %r" % s.out_text()
        s.kill9()
        rows = _rows(bin_path, d)
        if rows != ["id", "3"]:
            return FAIL, ("committed row lost after SIGKILL: %r" % rows)
        return PASS, "acknowledged row survives SIGKILL"
    finally:
        if s:
            s.close()
        remove_data_dir(d)


def sp_control_chars(bin_path):
    d = new_data_dir()
    try:
        rc, out, err = run_batch(
            bin_path, d,
            "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(32));\n"
            "INSERT INTO t VALUES (1,'tab\\there');\n"
            "INSERT INTO t VALUES (2,'line\\nbreak');\n"
            "SELECT id,s FROM t ORDER BY id;\n")
        text = out.decode("utf-8", "backslashreplace")
        lines = text.splitlines()
        extra_col = any(line == "1\ttab\there" for line in lines)
        split_row = "2\tline" in lines and "break" in lines
        if extra_col or split_row:
            return QUIRK, ("VARCHAR values containing TAB/newline corrupt the "
                           "TSV/lines protocol: tab->extra column, newline->"
                           "extra row. observed %r" % lines)
        return PASS, ""
    finally:
        remove_data_dir(d)


def sp_dash_no_space(bin_path):
    d = new_data_dir()
    try:
        rc, out, err = run_batch(
            bin_path, d,
            "CREATE TABLE t (id BIGINT PRIMARY KEY);\n"
            "SELECT id FROM t;--no-space-comment\n")
        got = strip_ready(out.decode("utf-8", "backslashreplace"))
        if any(line.startswith("ERROR") for line in got):
            return QUIRK, ("'--' without following whitespace is not a comment "
                           "(MySQL-compatible) -> %r" % got)
        return PASS, ""
    finally:
        remove_data_dir(d)


def sp_long_statement(bin_path):
    d = new_data_dir()
    try:
        value = "x" * 9000  # crosses the 8 KiB read chunk size
        run_batch(bin_path, d,
                  "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(20000));\n")
        rc, out, err = run_batch(
            bin_path, d,
            "INSERT INTO t VALUES (1,'" + value + "');\n"
            "SELECT id FROM t;\n")
        got = strip_ready(out.decode("utf-8", "backslashreplace"))
        if got != ["id", "1"]:
            return FAIL, "long statement failed: %r stderr=%r" % (got, err[:200])
        return PASS, "9 KB statement assembled across reads"
    finally:
        remove_data_dir(d)


def sp_unknown_arg(bin_path):
    d = new_data_dir()
    s = None
    try:
        s = Session(bin_path, d, args=["--platform-unknown-flag", "value"])
        s.wait_lines(1, READY_TIMEOUT)
        lines = s.out_lines()
        err = s.err_text()
        if lines[:1] != ["ready"]:
            return FAIL, "startup output %r" % lines
        if "warning:" not in err:
            return FAIL, "no warning on stderr for unknown flag: %r" % err
        if any("warning:" in ln for ln in lines):
            return FAIL, "warning leaked to stdout"
        return PASS, "unknown flags warn on stderr and startup continues"
    finally:
        if s:
            s.close()
        remove_data_dir(d)


def sp_bad_data_dir(bin_path):
    root = new_data_dir()
    path = os.path.join(root, "not-a-dir")
    with open(path, "w") as f:
        f.write("x")
    try:
        rc, out, err = run_batch(bin_path, path, "")
        if rc != 1:
            return FAIL, "rc=%r for a data dir that is a file" % rc
        if out != b"":
            return FAIL, "stdout not empty on startup failure: %r" % out
        if b"startup failed" not in err:
            return FAIL, "missing startup error on stderr: %r" % err
        return PASS, ""
    finally:
        remove_data_dir(root)


def sp_split_leading_keyword(bin_path):
    d = new_data_dir()
    s = None
    try:
        _prepare(bin_path, d)
        run_batch(bin_path, d, "INSERT INTO t VALUES (1);\n")
        s = Session(bin_path, d)
        s.wait_lines(1, READY_TIMEOUT)
        s.take_out()
        # A read boundary inside the leading keyword must not be parsed.
        s.send("SEL")
        s.pump(0.3)
        interim = s.take_out()
        s.send("ECT id FROM t;")
        s.close_stdin()
        code = s.wait(2.5)
        s.pump(0.2)
        got = (interim + s.take_out()).splitlines()
        if got == ["id", "1"]:
            return PASS, "partial keyword is buffered until the statement ends"
        return FAIL, ("a read boundary inside a leading keyword was parsed as "
                      "a complete statement; observed %r" % got)
    finally:
        if s and s.alive():
            s.kill9()
        if s:
            s.close()
        remove_data_dir(d)


def sp_no_final_newline(bin_path):
    d = new_data_dir()
    try:
        rc, out, err = run_batch(
            bin_path, d,
            "CREATE TABLE t (id BIGINT PRIMARY KEY);\n"
            "INSERT INTO t VALUES (1);\n"
            "SELECT id FROM t;")  # no trailing newline, stdin then closes
        got = strip_ready(out.decode("utf-8", "backslashreplace"))
        if got != ["id", "1"]:
            return FAIL, fail_detail(["id", "1"], got)
        return PASS, ""
    finally:
        remove_data_dir(d)


# --------------------------------------------------------------------------
# reporting
# --------------------------------------------------------------------------
LEGEND = ("P=pass F=fail Q=quirk(s) . = not applicable   "
          "columns: sc=semicolon default; +A=--dml-ack; +M=custom ready marker; "
          "ln=--statement-mode line")


def print_matrix(bin_path, configs, results):
    names = [key for key, _ in CORE_PROBES]
    header = "probe".ljust(22) + "".join(c["key"].rjust(9) for c in configs)
    print("binary: %s" % bin_path)
    print(header)
    print("-" * len(header))
    details = []
    for name in names:
        row = name.ljust(22)
        for cfg in configs:
            status, detail = results[(name, cfg["key"])]
            row += CELL[status].rjust(9)
            if status in (FAIL, QUIRK) and detail:
                details.append((name, cfg["key"], status, detail))
        print(row)
    print()
    if details:
        print("  details (config, status, observation):")
        for name, key, status, detail in details:
            print("  %-7s %-16s [%s] %s" % (status, name, key, detail))
        print()
    return details


def print_special(bin_path, rows):
    print("special probes (default semicolon mode unless stated):")
    for name, status, detail in rows:
        print("  %-4s %-22s %s" % (CELL.get(status, status), name, detail))
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", action="append", default=None,
                    help="binary path; repeat for several (default: "
                         "./build/minivectordb and ./build-lead/minivectordb "
                         "when present)")
    ap.add_argument("--quick", action="store_true",
                    help="only run the first four configurations")
    args = ap.parse_args()

    bins = args.bin
    if not bins:
        bins = [p for p in ("./build/minivectordb", "./build-lead/minivectordb")
                if os.path.exists(p)]
    if not bins:
        raise SystemExit("no binary found; pass --bin PATH")

    configs = make_configs()
    if args.quick:
        configs = configs[:4]

    all_results = {}
    exit_code = 0
    for bin_path in bins:
        if not os.path.exists(bin_path):
            raise SystemExit("binary not found: " + bin_path)
        print("=" * 78)
        print("protocol matrix: %s" % bin_path)
        print(LEGEND)
        print()
        results = {}
        for cfg in configs:
            for name, fn in CORE_PROBES:
                ctx = Ctx(bin_path, cfg)
                try:
                    status, detail = fn(ctx)
                except Exception as exc:  # keep the matrix going
                    status, detail = FAIL, "harness exception: %r" % (exc,)
                finally:
                    ctx.cleanup()
                results[(name, cfg["key"])] = (status, detail)
                if status == FAIL:
                    exit_code = 1
        details = print_matrix(bin_path, configs, results)
        all_results[bin_path] = results

        sp_rows = []
        for name, fn in special_probes():
            try:
                status, detail = fn(bin_path)
            except Exception as exc:
                status, detail = FAIL, "harness exception: %r" % (exc,)
            sp_rows.append((name, status, detail))
            if status == FAIL:
                exit_code = 1
        print_special(bin_path, sp_rows)
        print()

    # differences between binaries
    if len(all_results) > 1:
        print("=" * 78)
        print("differences between binaries")
        bin_names = list(all_results)
        found = False
        for cfg in configs:
            for name, _ in CORE_PROBES:
                statuses = [all_results[b][(name, cfg["key"])][0] for b in bin_names]
                if len(set(statuses)) > 1:
                    found = True
                    print("  %-16s [%-8s] " % (name, cfg["key"])
                          + "  ".join("%s=%s" % (os.path.basename(b), CELL.get(s, s))
                                      for b, s in zip(bin_names, statuses)))
        if not found:
            print("  none")
        print()

    print("=" * 78)
    print("assumptions from docs/PROTOCOL.md that this matrix CANNOT confirm")
    print("without the official driver:")
    for line in [
        "- the exact ready-marker string the driver waits for (and whether it",
        "  expects a prompt before it); empty marker is supported but untested",
        "  against the platform.",
        "- whether the driver needs --dml-ack and which affected counts it",
        "  expects; here DDL/BEGIN/COMMIT also emit 'OK 0'.",
        "- whether the driver reads SQL errors from stdout (current behavior)",
        "  or stderr, and whether ERROR text must match MySQL wording.",
        "- the send pattern: whole script at once vs one statement per write vs",
        "  one line per write. Framing/error-isolation results depend on it.",
        "- the exact SELECT wire format: header line, TAB separators, %.9g",
        "  distances, no zero-row sentinel, and no quoting/escaping of",
        "  embedded TAB/newline/NUL in VARCHAR values.",
        "- --statement-mode line semantics for statements continued on the next",
        "  line (ORDER BY / WHERE on later lines).",
        "- EOF vs EXIT/QUIT expectations, process exit codes after SQL errors",
        "  (currently 0), and whether SIGTERM may execute pending input.",
        "- ordering of SELECT ... LIMIT without ORDER BY (unordered map order in",
        "  memory mode, WAL order in disk mode).",
    ]:
        print(line)
    print()
    print("exit status: %d" % exit_code)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
