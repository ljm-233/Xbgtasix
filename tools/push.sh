#!/usr/bin/env bash
# 用 GITHUB_TOKEN 一次性推送 —— token 只从环境变量读，不回显、不落盘、不写进 git config。
#
# 用法：
#   export GITHUB_TOKEN=ghp_xxxxxxxx          # 或 github_pat_xxx（fine-grained）
#   ./tools/push.sh <owner/repo>              # 例如 ItQianChen/Xbgtasix
#
# 可选：
#   BRANCH=main ./tools/push.sh owner/repo    # 指定远端分支
#
# 安全说明：
#   - 脚本内部 set +x，不会把含 token 的命令回显到终端
#   - 推送用一次性 URL，推完 remote 重置成不带凭据的 https 地址
#   - 建议给 token 设最短有效期，用完在 GitHub 上 revoke
set -uo pipefail
cd "$(dirname "$0")/.."

REPO=${1:-}
if [ -z "$REPO" ]; then
  echo "用法: export GITHUB_TOKEN=<token> && ./tools/push.sh <owner/repo>"
  exit 2
fi
if [ -z "${GITHUB_TOKEN:-}" ]; then
  echo "缺少环境变量 GITHUB_TOKEN"
  echo "  做法: export GITHUB_TOKEN=ghp_xxx   （只在你自己的终端里）"
  exit 2
fi
BRANCH=${BRANCH:-main}

set +x
URL="https://x-access-token:${GITHUB_TOKEN}@github.com/${REPO}.git"

echo "== 将要推送到 github.com/${REPO}（分支 ${BRANCH}）"
git log --oneline -1
echo

if ! git push "$URL" "HEAD:${BRANCH}"; then
  echo
  echo "推送失败。常见原因："
  echo "  - token 权限不足（fine-grained 需要该仓库的 Contents: Read and write）"
  echo "  - token 过期或被 revoke"
  echo "  - 远端仓库还没创建（先在 GitHub 上建空仓库，不要初始化 README）"
  echo "  - 本机没有出网能力（curl https://github.com 返回 000）"
  exit 1
fi

# 推完把 remote 换成干净地址，避免 token 留在 .git/config
git remote remove origin >/dev/null 2>&1 || true
git remote add origin "https://github.com/${REPO}.git"
echo
echo "== 完成"
echo "   remote 已重置为: $(git remote get-url origin)"
echo "   提交数: $(git rev-list --count HEAD)"
echo "   提示：token 用完建议在 GitHub 上 revoke，或至少设过期时间"
