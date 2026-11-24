#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""运行时合规闸：派生数据不许落回源码树。

静态闸（tools/policy.py）只管"进 git"。可运行时的缓存、转码产物、prepared image、
shader cache、crash dump 照样会落进仓库目录——尤其是 clone 到的路径和编译路径不一致
的时候。所以**每个写盘的工具都要先过 ensure_outside_repo(dest)**：这是纪律，不是可选
装饰。

三个巧思（照抄一份成熟实现，别自己发明）：

  a) **标记文件 + 祖先检查**识别"任意克隆"，而不是硬编码 D:\\GTA6\\Xbgtasix：
     仓库根放一个 `.xbgtasix-root`，于是任何 clone、符号链接、不同盘符、被复制到别处
     的整棵树都认得出来。

  b) **目标文件还不存在也要能判**：canonicalize 最长的存在前缀，再把不存在的尾巴
     拼回去。这样 `build/gdk/include_clean`（还没建）和 `C:/tmp/not-yet/out` 都能判。

  c) **词法规范化**（消 `.`、pop 解 `..`），所以 `tools/../not-yet/out` 这种
     "从仓库内部绕出去又绕回来"的路径也会被拦住——这是一个单测。

放行：仓库里的 `build/`。理由见 docs/asset-policy.md —— 本仓库的构建产物就在仓库
目录内（build.sh 的 `OUT=build`），不放行整条构建链都会被拦。这是**显式取舍**，
不是遗忘：除 `<仓库根>/build/` 之外的任何仓库内落点一律拒绝。

用法：
    python tools/repo_guard.py <dest> [<dest>...]   # 0 = 可写；1 = 被拦；2 = 用法错误
    python tools/repo_guard.py --selftest
    python tools/repo_guard.py --print-root

Python 调用方：
    from repo_guard import ensure_outside_repo, InsideRepoError
    dest = ensure_outside_repo(dest)      # 返回规范化后的绝对路径；仓库内抛 InsideRepoError
"""
from __future__ import annotations

import os
import sys

ROOT_MARKER = ".xbgtasix-root"
BUILD_DIR = "build"          # 仓库内唯一放行的子目录（构建产物）
POLICY_DOC = "docs/asset-policy.md"
HERE = os.path.dirname(os.path.abspath(__file__))
TREE_ROOT = os.path.dirname(HERE)


class RepoGuardError(Exception):
    """运行时闸的统一异常基类：调用方只 catch 这一个就够了。"""


class BadDestError(RepoGuardError):
    """目标路径本身不合法（空、无法解析）。调用方写错了，不是策略问题。"""


class InsideRepoError(RepoGuardError):
    """落点在仓库内（且不在 build/ 放行区）。"""

    def __init__(self, dest, repo_root, resolved, marker=ROOT_MARKER):
        self.dest = str(dest)
        self.repo_root = repo_root
        self.resolved = resolved
        self.marker = marker
        super().__init__(self._message())

    def _message(self):
        return (
            "落点在仓库内：%s\n"
            "  仓库根: %s（由标记 %s 识别）\n"
            "  拒绝原因: 派生数据/下载物/缓存写进源码树，就会跟着 `git add -A` 进历史；\n"
            "            历史里的数据删一个 commit 是不够的（要 git filter-repo + force-push）。\n"
            "  改法: 写到仓库外，例如 %s；仓库内唯一放行的是 %s 目录（构建产物）。"
            % (self.resolved, self.repo_root, self.marker,
               os.path.join(os.path.dirname(self.repo_root), "xbgtasix-work"),
               os.path.join(self.repo_root, BUILD_DIR))
        )


# ---------------------------------------------------------------- 核心：路径解析
def _absolute(dest, base=None):
    """相对路径绝对化。**不做词法折叠**：把 `..` 留给 realpath 按内核语义解析
    （`link/..` 不等于 `link` 的父目录——先解链接再退一级）。"""
    if dest is None:
        raise BadDestError("目标路径为空")
    p = os.fsdecode(dest) if isinstance(dest, bytes) else str(dest)
    if not p.strip():
        raise BadDestError("目标路径为空")
    if not os.path.isabs(p):
        p = os.path.join(os.getcwd() if base is None else base, p)
    return p


def _realpath_if_exists(path):
    """存在就 realpath（解符号链接），不存在/无权限/非法返回 None。"""
    try:
        return os.path.realpath(path, strict=True)
    except TypeError:                                      # Python < 3.10 没有 strict
        return os.path.realpath(path) if os.path.exists(path) else None
    except OSError:
        return None


def _canonicalize_allow_missing(path):
    """canonicalize 最长的存在前缀，再把不存在的尾巴拼回去，最后词法规范化。"""
    rest = []
    cur = path
    while True:
        canon = _realpath_if_exists(cur)
        if canon is not None:
            out = os.path.join(canon, *reversed(rest)) if rest else canon
            return os.path.normpath(out)
        parent = os.path.dirname(cur)
        if not parent or parent == cur:                     # 到根还不存在（如不存在的盘符）
            return os.path.normpath(path)                   # 退化成纯词法，不抛错
        rest.append(os.path.basename(cur))
        cur = parent


def resolve(dest, base=None):
    """把 dest 规范化成绝对路径，**允许目标还不存在**。

    三步：绝对化（相对路径基于 base/cwd）→ canonicalize 最长的存在前缀（解析符号链接、
    按内核语义解 `..`）→ 拼回不存在的尾巴 → 词法规范化（消 `.`、pop `..`）。
    """
    return _canonicalize_allow_missing(_absolute(dest, base))


def _find_root(resolved, marker=ROOT_MARKER):
    """在已规范化的路径上向上找标记文件。"""
    cur = resolved
    while True:
        if os.path.isfile(os.path.join(cur, marker)):
            return cur
        parent = os.path.dirname(cur)
        if not parent or parent == cur:
            return None
        cur = parent


def find_repo_root(path, marker=ROOT_MARKER):
    """向上找 ROOT_MARKER；命中返回仓库根（已规范化），否则 None。"""
    return _find_root(resolve(path), marker)


def is_within(child, parent):
    """child 是否等于 parent 或在 parent 之内。按路径分量比较，不是字符串前缀。"""
    c = os.path.normcase(os.path.normpath(child))
    p = os.path.normcase(os.path.normpath(parent))
    if c == p:
        return True
    try:
        return os.path.commonpath([c, p]) == p
    except ValueError:                                     # 不同盘符
        return False


# ---------------------------------------------------------------- 对外 API
def ensure_outside_repo(dest, allow_build=True, base=None, marker=ROOT_MARKER):
    """校验落点；返回规范化后的绝对路径（调用方应该拿返回值去写盘）。

    合规 -> 返回 resolved；落点在仓库内 -> 抛 InsideRepoError；
    dest 本身不合法 -> 抛 BadDestError。
    """
    resolved = resolve(dest, base)
    root = _find_root(resolved, marker)
    if root is None:
        return resolved                                    # 仓库外，放行
    if allow_build and is_within(resolved, os.path.join(root, BUILD_DIR)):
        return resolved                                    # 仓库内的 build/：构建产物，显式放行
    raise InsideRepoError(dest, root, resolved, marker)


def is_outside_repo(dest, allow_build=True, base=None, marker=ROOT_MARKER):
    """布尔版本（不抛异常），给"能用就用、不能用就换地方"的调用方。"""
    try:
        ensure_outside_repo(dest, allow_build=allow_build, base=base, marker=marker)
        return True
    except RepoGuardError:
        return False


def explain(dest, allow_build=True, base=None, marker=ROOT_MARKER):
    """诊断用：返回 (resolved, 人类可读结论)，不抛策略异常。"""
    resolved = resolve(dest, base)
    root = _find_root(resolved, marker)
    if root is None:
        return resolved, "OK：仓库外，可以写"
    if allow_build and is_within(resolved, os.path.join(root, BUILD_DIR)):
        return resolved, ("OK：仓库内 %s/ 是构建产物目录，显式放行（理由见 docs/asset-policy.md）"
                          % BUILD_DIR)
    err = InsideRepoError(dest, root, resolved, marker)
    return resolved, "BLOCKED：" + err._message()


# ---------------------------------------------------------------- 自测
class _Skip(Exception):
    """本机不具备该能力（例如无权限建符号链接）-> SKIP，不算失败。"""


def _make_dir_link(link, target):
    """造一个"指向目录"的链接：优先 os.symlink；Windows 上没权限就退到 junction。

    为什么要退：Windows 建符号链接需要管理员/开发者模式，而 junction 普通用户就能建。
    两者都是"路径看着在临时目录、实际指回仓库"，正是闸门要处理的形状。
    """
    try:
        os.symlink(target, link, target_is_directory=True)
        return True
    except (OSError, NotImplementedError, AttributeError):
        pass
    if os.name == "nt":
        import subprocess
        proc = subprocess.run(["cmd", "/c", "mklink", "/J", link, target],
                              capture_output=True, text=True)
        return proc.returncode == 0 and os.path.isdir(link)
    return False


def _remove_dir_link(link):
    """只删链接本身，绝不跟进去删目标。"""
    import subprocess
    try:
        if os.path.islink(link):
            os.unlink(link)
            return
    except OSError:
        pass
    if os.path.isdir(link):
        subprocess.run(["cmd", "/c", "rmdir", link], capture_output=True)


def _repo():
    root = find_repo_root(HERE)
    if root is None:
        raise AssertionError("找不到仓库根标记 %s（本测试需要真实的仓库布局）" % ROOT_MARKER)
    return root


def _expect_blocked(name, dest, **kw):
    try:
        got = ensure_outside_repo(dest, **kw)
    except InsideRepoError:
        return
    raise AssertionError("%s：应当被拦，实际放行 -> %s" % (name, got))


def _expect_ok(name, dest, **kw):
    try:
        got = ensure_outside_repo(dest, **kw)
    except RepoGuardError as e:
        raise AssertionError("%s：应当放行，实际被拦 -> %s" % (name, e))
    if not os.path.isabs(got):
        raise AssertionError("%s：返回值必须是绝对路径，实际 %r" % (name, got))
    return got


def selftest_cases(tmp):
    """(用例名, 可调用对象)。可调用对象无事返回，失败抛 AssertionError。"""
    repo = _repo()
    work = os.path.join(os.path.dirname(repo), "xbgtasix-work")

    def reject_inside():
        _expect_blocked("仓库内路径", os.path.join(repo, "src", "x.cpp"))

    def reject_root():
        _expect_blocked("仓库根本身", repo)

    def reject_dotdot_escape():
        # tools/../not-yet-created/out：先出到仓库根，再进一个还不存在的目录
        _expect_blocked(".. 绕出去又绕回仓库",
                        os.path.join(repo, "tools", "..", "not-yet-created", "out"))

    def reject_dotdot_nonexistent():
        # 整个尾巴都不存在：靠"最长存在前缀 + 拼尾巴"仍要判出在仓库内
        _expect_blocked("不存在的尾巴也在仓库内",
                        os.path.join(repo, "no-such-dir", "a", "b", "c", "out.bin"))

    def reject_absolute():
        _expect_blocked("绝对路径在仓库内",
                        os.path.abspath(os.path.join(repo, "abs", "x.dat")))

    def reject_trailing_slash():
        _expect_blocked("带尾斜杠", os.path.join(repo, "src") + os.sep)

    def reject_build_prefix_trap():
        # "build-old" 不能因为字符串前缀像 "build" 就放行
        _expect_blocked("build 前缀陷阱（build-old/）",
                        os.path.join(repo, "build-old", "out.bin"))

    def accept_build():
        got = _expect_ok("build/ 放行", os.path.join(repo, "build", "gdk", "include_clean"))
        if not is_within(got, os.path.join(repo, BUILD_DIR)):
            raise AssertionError("build/ 返回值不在 build/ 里: %s" % got)

    def accept_build_deep_missing():
        _expect_ok("build/ 下深层不存在的路径",
                   os.path.join(repo, "build", "x", "y", "z", "cache.bin"))

    def build_can_be_turned_off():
        _expect_blocked("allow_build=False 时 build/ 被拦",
                        os.path.join(repo, "build", "gdk"), allow_build=False)

    def accept_outside_tmp():
        _expect_ok("仓库外临时目录", os.path.join(tmp, "not-yet", "out"))

    def accept_dotdot_in_missing_tail():
        _expect_ok("不存在的尾巴里有 ..", os.path.join(tmp, "nonexistent", "..", "out"))

    def accept_sibling_outside():
        _expect_ok("仓库的同级目录（仓库外）", os.path.join(work, "cache", "shader"))

    def accept_relative_from_cwd():
        old = os.getcwd()
        os.chdir(tmp)
        try:
            got = _expect_ok("相对路径（基于 cwd）", os.path.join("nested", "out.bin"))
        finally:
            os.chdir(old)
        if not got.startswith(os.path.realpath(tmp)):
            raise AssertionError("相对路径解析错: %s" % got)

    def accept_existing_file():
        target = os.path.join(tmp, "already-there.bin")
        open(target, "wb").write(b"x")
        _expect_ok("已存在的文件（仓库外）", target)

    def reject_existing_file_inside():
        _expect_blocked("已存在的文件（仓库内）", os.path.join(repo, "README.md"))

    def reject_symlink_into_repo():
        link = os.path.join(tmp, "link-into-repo")
        if not _make_dir_link(link, repo):
            raise _Skip("本机既不能建符号链接，也不能建 junction")
        try:
            _expect_blocked("符号链接/junction 指回仓库", os.path.join(link, "src", "x.cpp"))
        finally:
            _remove_dir_link(link)

    def reject_copied_tree():
        # 整棵树被复制到别处：标记跟着走，仍然认得出
        clone = os.path.join(tmp, "clone-of-repo")
        os.makedirs(os.path.join(clone, "src"), exist_ok=True)
        open(os.path.join(clone, ROOT_MARKER), "wb").close()
        open(os.path.join(clone, "src", "x.cpp"), "wb").write(b"int main(){}\n")
        _expect_blocked("被复制到别处的整棵树", os.path.join(clone, "src", "out.bin"))

    def tolerate_other_drive():
        if os.name != "nt":
            raise _Skip("只有 Windows 才有盘符")
        other = "C:\\Windows\\Temp\\xbgtasix-guard-probe\\out"
        got = _expect_ok("不同盘符（C:）", other)
        if not got.lower().startswith("c:"):
            raise AssertionError("盘符丢了: %s" % got)

    def bool_api():
        if not is_outside_repo(os.path.join(tmp, "a")):
            raise AssertionError("is_outside_repo 对仓库外应为 True")
        if is_outside_repo(os.path.join(repo, "src", "a.cpp")):
            raise AssertionError("is_outside_repo 对仓库内应为 False")
        if not is_outside_repo(os.path.join(repo, BUILD_DIR, "a.bin")):
            raise AssertionError("is_outside_repo 对 build/ 应为 True（放行）")

    def explain_api():
        got, verdict = explain(os.path.join(repo, "src", "x.cpp"))
        if "BLOCKED" not in verdict:
            raise AssertionError("explain 没报 BLOCKED: %r" % verdict)
        got2, verdict2 = explain(os.path.join(repo, BUILD_DIR, "a.bin"))
        if "build" not in verdict2:
            raise AssertionError("explain 没说明 build/ 放行: %r" % verdict2)

    def empty_dest():
        try:
            ensure_outside_repo("")
        except BadDestError:
            return
        except RepoGuardError as e:
            raise AssertionError("空路径应抛 BadDestError，实际 %s" % type(e).__name__)
        raise AssertionError("空路径居然通过了")

    def message_is_actionable():
        try:
            ensure_outside_repo(os.path.join(repo, "src", "x.cpp"))
        except InsideRepoError as e:
            text = str(e)
            for needle in (ROOT_MARKER, BUILD_DIR, "仓库内"):
                if needle not in text:
                    raise AssertionError("错误信息缺少 %r：\n%s" % (needle, text))
            return
        raise AssertionError("没被拦")

    return [
        ("拒绝：仓库内路径", reject_inside),
        ("拒绝：仓库根本身", reject_root),
        ("拒绝：.. 绕出去又绕回仓库", reject_dotdot_escape),
        ("拒绝：不存在的尾巴也在仓库内", reject_dotdot_nonexistent),
        ("拒绝：绝对路径在仓库内", reject_absolute),
        ("拒绝：带尾斜杠的仓库内路径", reject_trailing_slash),
        ("拒绝：build 前缀陷阱（build-old/）", reject_build_prefix_trap),
        ("拒绝：已存在的文件（仓库内）", reject_existing_file_inside),
        ("拒绝：allow_build=False 时 build/ 被拦", build_can_be_turned_off),
        ("接受：build/ 放行", accept_build),
        ("接受：build/ 下深层不存在的路径", accept_build_deep_missing),
        ("接受：仓库外临时目录", accept_outside_tmp),
        ("接受：不存在的尾巴里有 ..", accept_dotdot_in_missing_tail),
        ("接受：仓库的同级目录", accept_sibling_outside),
        ("接受：相对路径（基于 cwd）", accept_relative_from_cwd),
        ("接受：已存在的文件（仓库外）", accept_existing_file),
        ("接受/拒绝：is_outside_repo 布尔", bool_api),
        ("接受/拒绝：explain 诊断", explain_api),
        ("边界：空路径 -> BadDestError", empty_dest),
        ("边界：错误信息可执行（含标记/build/仓库内）", message_is_actionable),
        ("拒绝：符号链接指回仓库", reject_symlink_into_repo),
        ("拒绝：被复制到别处的整棵树", reject_copied_tree),
        ("接受：不同盘符不崩", tolerate_other_drive),
    ]


def run_selftest():
    import tempfile

    fails = 0
    skipped = 0
    cases = []
    print("== repo_guard 自测")
    tmp = tempfile.mkdtemp(prefix="xbgtasix-guard-")
    try:
        cases = selftest_cases(tmp)
        for name, fn in cases:
            try:
                fn()
            except _Skip as e:
                skipped += 1
                print("  SKIP  %s（%s）" % (name, e))
            except AssertionError as e:
                fails += 1
                print("  FAIL  %s" % name)
                for line in str(e).splitlines():
                    print("          %s" % line)
            except Exception as e:                      # noqa: BLE001 - 异常也要变红
                fails += 1
                print("  FAIL  %s" % name)
                print("          %s: %s" % (type(e).__name__, e))
            else:
                print("  PASS  %s" % name)
        # 结构性检查：本文件必须能真的识别当前仓库（否则闸门等于没装）
        repo = _repo()
        if not os.path.isfile(os.path.join(repo, ROOT_MARKER)):
            fails += 1
            print("  FAIL  当前仓库识别")
        else:
            print("  PASS  当前仓库识别 -> %s" % repo)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("\n自测: %d 个用例，失败 %d 个，跳过 %d 个 -> %s"
          % (len(cases), fails, skipped, "PASS" if fails == 0 else "FAIL"))
    return 0 if fails == 0 else 1


# ---------------------------------------------------------------- CLI
def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:                                  # noqa: BLE001
            pass

    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0 if args else 2
    if args[0] == "--selftest":
        return run_selftest()
    if args[0] == "--print-root":
        print(find_repo_root(HERE) or "")
        return 0

    allow_build = True
    if args[0] in ("--no-allow-build", "--allow-build"):
        allow_build = args[0] == "--allow-build"
        args = args[1:]
    if not args:
        print("错误：没有给路径", file=sys.stderr)
        return 2

    status = 0
    for dest in args:
        try:
            resolved, verdict = explain(dest, allow_build=allow_build)
        except BadDestError as e:
            print("用法错误：%s（%r）" % (e, dest))
            status = 2
            continue
        print("%s  %s" % ("BLOCKED" if "BLOCKED" in verdict else "OK     ", resolved))
        if "BLOCKED" in verdict:
            print("  " + verdict.replace("\n", "\n  "))
            status = max(status, 1)
    return status


if __name__ == "__main__":
    sys.exit(main())
