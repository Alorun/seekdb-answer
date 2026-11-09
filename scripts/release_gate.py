#!/usr/bin/env python3
"""Release gate for MiniVectorDB submissions.

Checks the repository/commit visibility, builds and smoke-tests the Docker image
and, when asked, validates agent/conversation.json.

Usage:
    python3 scripts/release_gate.py --require-conversation
    python3 scripts/release_gate.py --skip-docker --allow-dirty
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BRANCH = "2026-competition-round2"
PUBLIC_URL = "https://github.com/Alorun/seekdb-answer.git"

SECRET_PATTERNS = [
    re.compile(r"github_pat_[A-Za-z0-9_]{20,}"),
    re.compile(r"ghp_[A-Za-z0-9]{30,}"),
    re.compile(r"AKIA[0-9A-Z]{16}"),
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    re.compile(r"xox[baprs]-[A-Za-z0-9-]{10,}"),
]


def run(cmd, cwd=ROOT, input_text=None, timeout=120, env=None):
    e = dict(os.environ)
    e["GIT_TERMINAL_PROMPT"] = "0"
    if env:
        e.update(env)
    return subprocess.run(
        cmd,
        cwd=cwd,
        input=input_text,
        text=True,
        capture_output=True,
        timeout=timeout,
        env=e,
    )


def fail(msg):
    print("FAIL: " + msg)
    return False


def check_git(allow_dirty):
    ok = True
    p = run(["git", "status", "--porcelain"])
    if p.returncode != 0:
        return fail("git status failed: " + p.stderr.strip())
    if p.stdout.strip() and not allow_dirty:
        ok = fail("working tree is dirty:\n" + p.stdout)
    p = run(["git", "rev-parse", "--abbrev-ref", "HEAD"])
    branch = p.stdout.strip()
    if branch != BRANCH:
        ok = fail(f"branch is {branch!r}, expected {BRANCH!r}")
    p = run(["git", "rev-parse", "HEAD"])
    sha = p.stdout.strip()
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        ok = fail(f"bad commit sha {sha!r}")
    print(f"commit: {sha}")
    print(f"branch: {branch}")
    return ok, sha


def strip_credentials(url):
    return re.sub(r"https://[^@/]+@", "https://", url.strip())


def check_remote(sha):
    # Use the public URL without local credentials, simulating the platform.
    p = subprocess.run(
        ["git", "-c", "credential.helper=", "ls-remote", PUBLIC_URL,
         f"refs/heads/{BRANCH}"],
        text=True,
        capture_output=True,
        timeout=60,
        env={**os.environ, "GIT_TERMINAL_PROMPT": "0"},
    )
    if p.returncode != 0:
        return fail("anonymous git ls-remote failed: " + p.stderr.strip())
    fields = p.stdout.split()
    if len(fields) < 1:
        return fail("branch not visible anonymously")
    if fields[0] != sha:
        return fail(f"remote branch points to {fields[0]}, expected {sha}")
    print("anonymous remote: ok")
    return True


def check_secrets():
    p = run(["git", "ls-files", "-z"])
    ok = True
    for raw in p.stdout.split("\0"):
        if not raw:
            continue
        path = os.path.join(ROOT, raw)
        try:
            with open(path, "rb") as f:
                data = f.read()
            if b"\0" in data:
                continue
            text = data.decode("utf-8", "ignore")
        except OSError:
            continue
        for pat in SECRET_PATTERNS:
            if pat.search(text):
                ok = fail(f"possible secret in {raw}: {pat.pattern}")
    return ok


def no_duplicate_pairs(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise ValueError(f"duplicate JSON key: {k}")
        out[k] = v
    return out


def check_conversation(path, required):
    if not os.path.exists(path):
        if required:
            return fail(f"missing {path}")
        print(f"warning: {path} missing (not required for this run)")
        return True
    size = os.path.getsize(path)
    if size > 128 * 1024:
        return fail(f"conversation.json is {size} bytes (>128 KiB)")
    try:
        text = open(path, "r", encoding="utf-8").read()
        data = json.loads(
            text,
            object_pairs_hook=no_duplicate_pairs,
            parse_constant=lambda x: (_ for _ in ()).throw(
                ValueError(f"non-standard JSON constant: {x}")
            ),
        )
    except Exception as e:
        return fail(f"conversation.json is not valid JSON: {e}")
    if not isinstance(data, dict):
        return fail("conversation.json top level must be an object")
    # JSON pointer resolvability for common evidence fields.
    def resolve(ptr):
        if not isinstance(ptr, str) or not ptr.startswith("/"):
            return False
        cur = data
        for part in ptr[1:].split("/"):
            part = part.replace("~1", "/").replace("~0", "~")
            if isinstance(cur, list):
                try:
                    cur = cur[int(part)]
                except (ValueError, IndexError):
                    return False
            elif isinstance(cur, dict):
                if part not in cur:
                    return False
                cur = cur[part]
            else:
                return False
        return True

    def walk(node):
        if isinstance(node, dict):
            for k, v in node.items():
                if k.endswith("_pointer") and isinstance(v, str):
                    if not resolve(v):
                        raise ValueError(f"unresolvable JSON pointer {v!r}")
                walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)

    try:
        walk(data)
    except Exception as e:
        return fail(f"conversation.json pointer check failed: {e}")
    digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
    print(f"conversation: {size} bytes sha256={digest}")
    return True


def run_container_smoke(image):
    tmp = tempfile.mkdtemp(prefix="mvdb-gate-")
    try:
        # Official public cases use one complete SQL per line (no semicolons)
        # and assert affected_rows/columns/rows, so the default protocol is
        # line framing plus an explicit DML status.
        sql = (
            "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), "
            "content VARCHAR(1024), embedding VECTOR(4))\n"
            "INSERT INTO docs VALUES (1,'database','事务与恢复','[1,0,0,0]'),"
            "(2,'database','向量索引','[0,1,0,0]'),(3,'travel','旅行攻略','[0,0,1,0]')\n"
            "SELECT id, content, L2_DISTANCE(embedding,'[0.9,0.1,0,0]') AS distance "
            "FROM docs WHERE category = 'database' ORDER BY distance LIMIT 2\n"
            "SELECT id, content FROM docs ORDER BY id\n"
        )
        p = run(
            ["docker", "run", "--rm", "-i", "--network", "none",
             "-v", f"{tmp}:/data", image],
            input_text=sql,
            timeout=120,
        )
        if p.returncode != 0:
            return fail("container smoke exited " + str(p.returncode) + ": " + p.stderr[-500:])
        for needle in ["id\tcontent\tdistance", "1\t事务与恢复", "2\t向量索引"]:
            if needle not in p.stdout:
                return fail("container smoke missing " + repr(needle))
        p2 = run(
            ["docker", "run", "--rm", "-i", "--network", "none",
             "-v", f"{tmp}:/data", image],
            input_text="SELECT id FROM docs ORDER BY id\n",
            timeout=60,
        )
        for needle in ["1", "2", "3"]:
            if needle not in p2.stdout:
                return fail("restart smoke missing row " + needle)
        # Forced disk mode must produce the same rows.
        tmp2 = tempfile.mkdtemp(prefix="mvdb-gate-disk-")
        try:
            p3 = run(
                ["docker", "run", "--rm", "-i", "--network", "none",
                 "-v", f"{tmp2}:/data", image, "--data-dir", "/data",
                 "--memory-rows", "0"],
                input_text=sql,
                timeout=120,
            )
            if p3.returncode != 0:
                return fail("disk-mode smoke exited " + str(p3.returncode))
            for needle in ["id\tcontent\tdistance", "1\t事务与恢复"]:
                if needle not in p3.stdout:
                    return fail("disk-mode smoke missing " + repr(needle))
        finally:
            shutil.rmtree(tmp2, ignore_errors=True)
        print("container smoke: ok")
        return True
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def check_docker(image):
    # Reuse Docker's layer cache: COPY src invalidates the build when the
    # database source changes, while the apt/toolchain layers need not be
    # downloaded again. This keeps the gate usable on slow CI networks.
    p = run(["docker", "build", "-t", image, "."], timeout=600)
    if p.returncode != 0:
        return fail("docker build failed: " + p.stderr[-1000:])
    print("docker build: ok")
    return run_container_smoke(image)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-docker", action="store_true")
    ap.add_argument("--allow-dirty", action="store_true")
    ap.add_argument("--require-conversation", action="store_true")
    ap.add_argument("--conversation", default=os.path.join(ROOT, "agent", "conversation.json"))
    ap.add_argument("--image-tag", default="minivectordb:gate")
    args = ap.parse_args()

    ok = True
    git_ok, sha = check_git(args.allow_dirty)
    ok &= git_ok
    if git_ok:
        ok &= check_remote(sha)
    ok &= check_secrets()
    ok &= check_conversation(args.conversation, args.require_conversation)
    if not args.skip_docker:
        ok &= check_docker(args.image_tag)
    print("RESULT: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
