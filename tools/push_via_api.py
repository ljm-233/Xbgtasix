#!/usr/bin/env python3
"""用 GitHub REST API 推送完整提交历史（当 git 的 TLS 后端不可用时）。

背景：某些环境下 git 的 schannel 会报 `AcquireCredentialsHandle failed`，
但 `gh api` 走的是 Go 自己的 TLS 栈，能通。于是用 API 逐个提交重放：
  blob -> tree(全量) -> commit(带 parent) -> 最后 PATCH ref

好处：**提交历史、提交信息、作者/时间都保留**，不是压成一个 squash 提交。

用法：
    python tools/push_via_api.py <owner/repo> [--branch main]
"""
import argparse
import base64
import glob
import json
import os
import shutil
import subprocess
import sys
import time

GH_CANDIDATES = [r"C:\Program Files\GitHub CLI\gh.exe", "gh"]


def find_gh():
    for c in GH_CANDIDATES:
        if os.path.sep in c:
            if os.path.isfile(c):
                return c
        else:
            p = shutil.which(c)
            if p:
                return p
    raise SystemExit("找不到 gh（GitHub CLI）")


GH = find_gh()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def git(*args, binary=False):
    r = subprocess.run(["git"] + list(args), cwd=ROOT, capture_output=True)
    if r.returncode != 0:
        raise SystemExit("git %s 失败: %s" % (" ".join(args), r.stderr.decode("utf-8", "replace")[:400]))
    return r.stdout if binary else r.stdout.decode("utf-8", "replace")


_body_path = os.path.join(ROOT, "build", "gh_api_body.json")


def api(method, path, body=None, retries=4):
    """带重试：网络中途被掐（stream error / CANCEL）在这类长推送里很常见。"""
    last = ""
    for attempt in range(1, retries + 1):
        try:
            return _api_once(method, path, body)
        except SystemExit as e:
            last = str(e)
            if attempt == retries:
                raise
            wait = 2 ** attempt
            print("    （第 %d 次失败，%ds 后重试）%s" % (attempt, wait, last.splitlines()[-1][:80]))
            time.sleep(wait)
    raise SystemExit(last)


def _api_once(method, path, body=None):
    cmd = [GH, "api", "--method", method, path]
    if body is not None:
        os.makedirs(os.path.dirname(_body_path), exist_ok=True)
        with open(_body_path, "w", encoding="utf-8") as f:
            json.dump(body, f)
        cmd += ["--input", _body_path]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        msg = r.stderr.decode("utf-8", "replace")[:600]
        raise SystemExit("gh api %s %s 失败:\n%s" % (method, path, msg))
    out = r.stdout.decode("utf-8", "replace").strip()
    return json.loads(out) if out else {}


def commit_meta(sha):
    raw = git("log", "-1", "--format=%an%x00%ae%x00%aI%x00%cn%x00%ce%x00%cI%x00%B", sha)
    parts = raw.split("\x00")
    return {
        "author": {"name": parts[0], "email": parts[1], "date": parts[2]},
        "committer": {"name": parts[3], "email": parts[4], "date": parts[5]},
        "message": parts[6],
    }


def tree_entries(sha):
    out = git("ls-tree", "-r", "-z", sha, binary=True).decode("utf-8", "surrogateescape")
    entries, blobs = [], []
    for item in out.split("\x00"):
        if not item:
            continue
        meta, path = item.split("\t", 1)
        mode, typ, obj = meta.split()
        if typ != "blob":
            continue
        entries.append({"path": path, "mode": mode, "type": "blob", "sha": obj})
        blobs.append((obj, path))
    return entries, blobs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("repo", help="owner/repo")
    ap.add_argument("--branch", default="main")
    ap.add_argument("--start-from", default="", help="从这个远端提交开始续传（默认全量）")
    a = ap.parse_args()

    commits = git("rev-list", "--reverse", "HEAD").split()
    print("本地提交 %d 个，开始重放到 %s（分支 %s）" % (len(commits), a.repo, a.branch))

    blob_cache = {}
    parent = None
    total_blobs = 0
    for i, sha in enumerate(commits, 1):
        entries, blobs = tree_entries(sha)
        tree_items = []
        for obj, path in blobs:
            if obj not in blob_cache:
                data = git("cat-file", "blob", obj, binary=True)
                created = api("POST", "/repos/%s/git/blobs" % a.repo, {
                    "content": base64.b64encode(data).decode("ascii"),
                    "encoding": "base64",
                })
                blob_cache[obj] = created["sha"]
                total_blobs += 1
            tree_items.append({"path": path, "mode": "100644", "type": "blob",
                               "sha": blob_cache[obj]})

        tree = api("POST", "/repos/%s/git/trees" % a.repo, {"tree": tree_items})
        meta = commit_meta(sha)
        body = {
            "message": meta["message"],
            "tree": tree["sha"],
            "author": meta["author"],
            "committer": meta["committer"],
        }
        if parent:
            body["parents"] = [parent]
        else:
            body["parents"] = []
        created = api("POST", "/repos/%s/git/commits" % a.repo, body)
        parent = created["sha"]
        print("  [%2d/%d] %s  %s" % (i, len(commits), sha[:7],
                                     meta["message"].splitlines()[0][:60]))

    api("PATCH", "/repos/%s/git/refs/heads/%s" % (a.repo, a.branch),
        {"sha": parent, "force": True})
    print("\n完成：%s 分支已指向 %s（上传 blob %d 个，去重后共 %d）" % (
        a.branch, parent[:7], total_blobs, len(blob_cache)))
    print("仓库: https://github.com/%s" % a.repo)


if __name__ == "__main__":
    sys.exit(main())
