"""Regression tests for the metrics ledger (incident PR fix/metrics-branch, 2026-08-29):
tools/metrics-ledger.sh and tools/metrics-report.py against a temporary bare remote.

The workflows that call the script only fire on real PR/issue events, so this is the
verification that runs on every push: the branch is created and pushed when absent,
appends fast-forward, "nothing to commit" is a distinct exit-0 path, and — the failure the
incident masked — a rejected push exits non-zero instead of printing "nothing to commit"."""
from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
LEDGER = ROOT / "tools" / "metrics-ledger.sh"
REPORT = ROOT / "tools" / "metrics-report.py"
IDENT = ["-c", "user.name=fixture", "-c", "user.email=fixture@example.invalid"]


def git(cwd, *args):
    return subprocess.run(["git", *IDENT, *args], cwd=cwd, capture_output=True, text=True, check=True).stdout


def run(cwd, *cmd, env_extra=None):
    env = dict(os.environ, **(env_extra or {}))
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


@pytest.fixture
def remote(tmp_path):
    """A bare origin with a `main` branch holding one commit, and a helper to clone it."""
    bare = tmp_path / "origin.git"
    subprocess.run(["git", "init", "-q", "--bare", str(bare)], check=True)
    seed = tmp_path / "seed"
    subprocess.run(["git", "init", "-q", "-b", "main", str(seed)], check=True)
    (seed / "README").write_text("seed\n")
    git(seed, "add", "README")
    git(seed, "commit", "-q", "-m", "seed")
    git(seed, "remote", "add", "origin", str(bare))
    git(seed, "push", "-q", "origin", "main")

    def clone(name):
        d = tmp_path / name
        subprocess.run(["git", "clone", "-q", "-b", "main", str(bare), str(d)], check=True)
        return d

    return bare, clone


def append(work, name, obj):
    (work / "metrics").mkdir(exist_ok=True)
    with (work / "metrics" / name).open("a") as fh:
        fh.write(json.dumps(obj) + "\n")


def remote_branch_sha(bare):
    out = subprocess.run(["git", "ls-remote", "--heads", str(bare), "refs/heads/metrics"],
                         capture_output=True, text=True).stdout.strip()
    return out.split("\t")[0] if out else None


def test_absent_branch_is_created_from_main_and_pushed(remote):
    bare, clone = remote
    work = clone("a")
    rc, out = run(work, str(LEDGER), "checkout")
    assert rc == 0 and "absent" in out and "created metrics" in out
    append(work, "delivery-log.jsonl", {"pr": 1})
    rc, out = run(work, str(LEDGER), "commit", "test[bot]", "metrics: record PR #1")
    assert rc == 0, out
    assert "origin/metrics ->" in out and "nothing to commit" not in out
    assert remote_branch_sha(bare) == git(work, "rev-parse", "HEAD").strip()
    shown = subprocess.run(["git", "-C", str(bare), "show", "metrics:metrics/delivery-log.jsonl"],
                           capture_output=True, text=True, check=True).stdout
    assert json.loads(shown) == {"pr": 1}


def test_second_runner_appends_and_fast_forwards(remote):
    bare, clone = remote
    a = clone("a")
    run(a, str(LEDGER), "checkout")
    append(a, "delivery-log.jsonl", {"pr": 1})
    assert run(a, str(LEDGER), "commit", "test[bot]", "one")[0] == 0
    b = clone("b")                       # a fresh runner: only main is checked out
    rc, out = run(b, str(LEDGER), "checkout")
    assert rc == 0 and "on metrics at" in out and "from origin" in out
    assert (b / "metrics" / "delivery-log.jsonl").read_text().count("\n") == 1
    append(b, "delivery-log.jsonl", {"pr": 2})
    rc, out = run(b, str(LEDGER), "commit", "test[bot]", "two")
    assert rc == 0, out
    log = subprocess.run(["git", "-C", str(bare), "log", "--format=%s", "metrics"],
                         capture_output=True, text=True, check=True).stdout.split()
    assert log[:2] == ["two", "one"]
    shown = subprocess.run(["git", "-C", str(bare), "show", "metrics:metrics/delivery-log.jsonl"],
                           capture_output=True, text=True, check=True).stdout
    assert [json.loads(l)["pr"] for l in shown.splitlines()] == [1, 2]


def test_nothing_to_commit_is_exit_zero_and_says_so(remote):
    bare, clone = remote
    work = clone("a")
    run(work, str(LEDGER), "checkout")
    rc, out = run(work, str(LEDGER), "commit", "test[bot]", "noop")
    assert rc == 0 and "nothing to commit" in out
    assert remote_branch_sha(bare) is None      # nothing was pushed either


def test_rejected_push_fails_loudly_instead_of_masking(remote):
    """The incident: the old `git push || echo "nothing to commit"` turned a rejected push
    into a green run. A non-fast-forward rejection must now exit non-zero."""
    bare, clone = remote
    a = clone("a")
    run(a, str(LEDGER), "checkout")
    append(a, "delivery-log.jsonl", {"pr": 1})
    assert run(a, str(LEDGER), "commit", "test[bot]", "one")[0] == 0
    b = clone("b")
    run(b, str(LEDGER), "checkout")          # b is at "one"
    # a advances the remote behind b's back …
    append(a, "delivery-log.jsonl", {"pr": 2})
    assert run(a, str(LEDGER), "commit", "test[bot]", "two")[0] == 0
    # … so b's push is rejected (non-fast-forward): must fail, must not say "nothing to commit".
    append(b, "delivery-log.jsonl", {"pr": 3})
    rc, out = run(b, str(LEDGER), "commit", "test[bot]", "three")
    assert rc != 0
    assert "rejected" in out and "nothing to commit" not in out
    assert remote_branch_sha(bare) == git(a, "rev-parse", "HEAD").strip()   # remote untouched by b


def test_ls_remote_failure_is_not_treated_as_absent_branch(remote, tmp_path):
    bare, clone = remote
    work = clone("a")
    rc, out = run(work, str(LEDGER), "checkout", env_extra={"METRICS_REMOTE": str(tmp_path / "no-such-remote")})
    assert rc == 1 and "not a missing branch" in out


DELIVERY_REC = {"pr": 15, "title": "t", "feature": "f1-codecs", "author_type": "agent",
                "created_at": "2026-08-29T00:00:00Z", "merged_at": "2026-08-29T12:00:00Z",
                "cycle_time_h": 12.0, "commits": 3, "changed_files": 4, "additions": 10,
                "deletions": 2, "ci_runs": 1, "first_pass_green": True, "review_rounds": 0,
                "changes_requested": False}


def test_report_distinguishes_no_branch_from_empty_branch(remote):
    bare, clone = remote
    work = clone("a")
    rc, out = run(work, sys.executable, str(REPORT))
    assert rc == 1 and "no `metrics` branch on origin yet" in out and "nothing has been recorded" in out
    # branch exists with a delivery log but no task log
    run(work, str(LEDGER), "checkout")
    append(work, "delivery-log.jsonl", DELIVERY_REC)
    assert run(work, str(LEDGER), "commit", "test[bot]", "one")[0] == 0
    other = clone("b")                       # reads purely from origin/metrics, no local files
    rc, out = run(other, sys.executable, str(REPORT))
    assert rc == 0, out
    assert "delivery log from origin/metrics:metrics/delivery-log.jsonl" in out
    assert "== ALL (1 PRs) ==" in out and "== agent-authored (1 PRs) ==" in out
    assert "origin/metrics exists but has no metrics/task-log.jsonl yet" in out
    rc, out = run(other, sys.executable, str(REPORT), "--source", "branch")
    assert rc == 0 and "== ALL (1 PRs) ==" in out


def test_report_local_fallback_names_itself(remote):
    bare, clone = remote
    work = clone("a")
    append(work, "delivery-log.jsonl", DELIVERY_REC)
    rc, out = run(work, sys.executable, str(REPORT))
    assert rc == 0 and "local metrics/delivery-log.jsonl (no `metrics` branch on origin yet; local fallback)" in out
    rc, out = run(work, sys.executable, str(REPORT), "--source", "branch")
    assert rc == 1 and "no `metrics` branch on origin yet" in out
