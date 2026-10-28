#!/bin/bash
# =============================================================================
# RIMES macOS 发布脚本与旧数据预览维护入口
#
# 正式目标来自 VERSION，发布身份来自 tag。脚本确认 origin/main 的 macOS
# 发布就绪检查通过，
# 在 origin/main 上创建 tag 并只推送这个 tag；不修改、不提交任何文件。
# tag 推送后由 GitHub Actions 构建、验证并创建 Release。
#
# 用法：
#   ./scripts/release.sh preview               # 继续当前预览线：v0.5.0-preview.1 → v0.5.0-preview.2
#   ./scripts/release.sh preview minor         # 开始新预览线：最新正式版 minor+1 的 preview.1
#   ./scripts/release.sh preview 0.6.0         # 开始指定的预览线 v0.6.0-preview.1
#   ./scripts/release.sh stable                # 发布 VERSION 锚定的正式版本：v1.0.0
#   ./scripts/release.sh patch|minor|major     # 直接发布正式版（基于最新正式版）
#   ./scripts/release.sh 0.5.1                 # 直接发布指定正式版
#   ./scripts/release.sh platform minor        # Windows / Linux 数据预览 platform-preview-vX.Y.Z
#   ./scripts/release.sh platform 0.2.0        # 指定跨平台预览版本
#
# 选项：
#   -n, --dry-run   只做校验并展示计划与发布说明；任何分支都可运行，不创建或推送 tag
#   -y, --yes       跳过交互确认（用于受控自动化）
#   -h, --help      显示本说明
#
# 正式版需要 Developer ID signing Environment 和独立的 publish Environment；
# 缺失时脚本拒绝发布。
# =============================================================================
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[OK]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
die()     { echo -e "${RED}[ERROR]${NC} $1" >&2; exit 1; }

usage() {
    sed -n '2,26p' "$0" | sed -E 's/^# ?//'
}

REMOTE="origin"
EXPECTED_REPO="scholay/xsime"
# Exact workflow IDs/events and versions are checked by release_guard.py.

github_repo_from_url() {
    local url="$1" repo
    case "$url" in
        git@github.com:*) repo="${url#git@github.com:}" ;;
        ssh://git@github.com/*) repo="${url#ssh://git@github.com/}" ;;
        https://github.com/*) repo="${url#https://github.com/}" ;;
        http://github.com/*) repo="${url#http://github.com/}" ;;
        *) return 1 ;;
    esac
    repo="${repo%/}"
    repo="${repo%.git}"
    printf '%s' "$repo" | tr '[:upper:]' '[:lower:]'
}

remote_tag_exists() {
    local output
    if ! output="$(git ls-remote --tags "$REMOTE" "refs/tags/$1" 2>/dev/null)"; then
        die "无法查询 $REMOTE 的远端标签；未执行任何发布操作。"
    fi
    [[ -n "$output" ]]
}

remote_main_sha() {
    local line
    line="$(git ls-remote --heads "$REMOTE" refs/heads/main 2>/dev/null)" \
        || die "无法查询 $EXPECTED_REPO 的 main 分支。"
    [[ -n "$line" ]] || die "$EXPECTED_REPO 不存在 main 分支。"
    printf '%s' "${line%%[[:space:]]*}"
}

# A gate either stops the release or, in a dry run, is reported and skipped so
# the plan can still be inspected from a feature branch.
FAILED_GATES=0
gate() {
    if [[ "$DRY_RUN" == true ]]; then
        warn "（dry-run 不阻断）$1"
        FAILED_GATES=$((FAILED_GATES + 1))
    else
        die "$1"
    fi
}

DRY_RUN=false
ASSUME_YES=false
POSITIONAL=()
while (( $# > 0 )); do
    case "$1" in
        -n|--dry-run) DRY_RUN=true ;;
        -y|--yes) ASSUME_YES=true ;;
        -h|--help) usage; exit 0 ;;
        --)
            shift
            POSITIONAL+=("$@")
            break
            ;;
        -*) die "未知选项: $1（使用 --help 查看用法）" ;;
        *) POSITIONAL+=("$1") ;;
    esac
    shift
done

(( ${#POSITIONAL[@]} > 0 )) || { usage; exit 1; }
COMMAND="${POSITIONAL[0]}"
ARG="${POSITIONAL[1]:-}"
(( ${#POSITIONAL[@]} <= 2 )) || die "参数过多（使用 --help 查看用法）。"

case "$COMMAND" in
    preview) KIND="preview" ;;
    stable)
        KIND="stable"
        [[ -z "$ARG" ]] || die "stable 默认采用 VERSION，不接受参数；直接发布指定正式版请用 ./scripts/release.sh X.Y.Z"
        ;;
    platform)
        KIND="platform"
        [[ -n "$ARG" ]] || die "用法: ./scripts/release.sh platform patch|minor|major|X.Y.Z"
        ;;
    patch|minor|major|[0-9]*)
        KIND="stable"
        [[ -z "$ARG" ]] || die "正式版只接受一个版本或升级类型。"
        ARG="$COMMAND"
        ;;
    *) die "未知命令: ${COMMAND}（使用 --help 查看用法）" ;;
esac

cd "$(dirname "$0")/.."
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "当前目录不是 Git 仓库。"
command -v python3 >/dev/null || die "需要 python3。"
command -v gh >/dev/null || die "需要 GitHub CLI（gh）来确认 CI 状态；请安装并 gh auth login。"
TOOL="scripts/release/release_tool.py"
PRODUCT_VERSION="$(cat VERSION)"
[[ "$PRODUCT_VERSION" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]] || die "VERSION 必须是正式产品版本。"
if [[ "$KIND" == stable && -z "$ARG" ]]; then
    ARG="$PRODUCT_VERSION"
fi

git remote get-url "$REMOTE" >/dev/null 2>&1 || die "缺少发布远端 ${REMOTE}。"
fetch_url="$(git remote get-url "$REMOTE")"
push_url="$(git remote get-url --push "$REMOTE")"
fetch_repo="$(github_repo_from_url "$fetch_url")" || die "$REMOTE fetch URL 不是 GitHub 地址: $fetch_url"
push_repo="$(github_repo_from_url "$push_url")" || die "$REMOTE push URL 不是 GitHub 地址: $push_url"
[[ "$fetch_repo" == "$EXPECTED_REPO" ]] || die "$REMOTE fetch URL 必须指向 ${EXPECTED_REPO}，实际为 ${fetch_repo}。"
[[ "$push_repo" == "$EXPECTED_REPO" ]] || die "$REMOTE push URL 必须指向 ${EXPECTED_REPO}，实际为 ${push_repo}。"

# --- Version plan -----------------------------------------------------------
remote_tags="$(git ls-remote --tags --refs "$REMOTE" 2>/dev/null)" \
    || die "无法读取 $EXPECTED_REPO 的远端标签；未执行任何发布操作。"
plan_status=0
plan_output="$(printf '%s\n' "$remote_tags" | python3 "$TOOL" next "$KIND" ${ARG:+"$ARG"} 2>&1)" \
    || plan_status=$?
(( plan_status == 0 )) || die "$plan_output"

TAG="" VERSION="" PREVIOUS=""
WARNINGS=()
while IFS= read -r line; do
    case "$line" in
        TAG=*) TAG="${line#TAG=}" ;;
        VERSION=*) VERSION="${line#VERSION=}" ;;
        PREVIOUS=*) PREVIOUS="${line#PREVIOUS=}" ;;
        WARNING=*) WARNINGS+=("${line#WARNING=}") ;;
    esac
done <<< "$plan_output"
[[ -n "$TAG" && -n "$VERSION" ]] || die "无法解析版本计划: $plan_output"
if [[ "$KIND" == stable && "$VERSION" != "$PRODUCT_VERSION" ]]; then
    die "正式版本 $VERSION 与 VERSION=$PRODUCT_VERSION 不一致；先更新并验证发布目标。"
fi

# --- Gates ------------------------------------------------------------------
fetch_main="$(remote_main_sha)"
git fetch --quiet "$REMOTE" main --tags || die "git fetch $REMOTE main 失败。"

branch="$(git symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
[[ "$branch" == "main" ]] || gate "只能从 main 分支发布，当前分支: ${branch:-detached HEAD}。"
if [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
    git status --short >&2
    gate "工作区不干净；发布脚本不会自动 add/commit。"
fi
[[ "$(git rev-parse HEAD)" == "$fetch_main" ]] \
    || gate "本地 HEAD 不是 origin/main（$(git rev-parse --short "$fetch_main")）。先 git pull；本地新提交请走 PR。"

remote_tag_exists "$TAG" && die "远端 tag $TAG 已存在；发布脚本绝不会删除或覆盖远端标签。"
git show-ref --verify --quiet "refs/tags/$TAG" && die "本地 tag $TAG 已存在；请先确认其来源，发布脚本不会重建标签。"

python3 "$TOOL" check-plist || gate "Info.plist 与 VERSION 发布目标不一致。"
if [[ "$KIND" != "platform" ]]; then
    python3 -B scripts/sync-buffer-plugin-catalog.py --check >/dev/null \
        || gate "预置缓冲插件 catalog / README 未同步。"
fi

info "确认 origin/main $(git rev-parse --short "$fetch_main") 的 CI 结果..."
guard_arguments=(ci --commit "$fetch_main")
if [[ "$KIND" != "stable" ]]; then
    guard_arguments+=(--workflow ci.yml)
fi
python3 scripts/release/release_guard.py "${guard_arguments[@]}" \
    || gate "确切 main 提交的原生 CI 未全部通过。"
if [[ "$KIND" == "stable" ]]; then
    android_build=$(python3 -c 'import re; from pathlib import Path; print(re.findall(r"versionCode = ([0-9]+)", Path("platforms/android/app/build.gradle.kts").read_text())[0])')
    python3 scripts/release/release_guard.py android-build --version "$VERSION" --build "$android_build" \
        || gate "正式版本或 Android versionCode 未高于已公开版本。"
fi

if [[ "$KIND" == "stable" ]]; then
    for formal_environment in macos-release macos-publish; do
        gh api "repos/$EXPECTED_REPO/environments/$formal_environment" >/dev/null 2>&1 \
            || gate "正式版需要受保护的 $formal_environment Environment；当前仓库尚未完整配置。"
        reviewer_rules="$(gh api "repos/$EXPECTED_REPO/environments/$formal_environment" \
            --jq '[.protection_rules[]? | select(.type == "required_reviewers" and ((.reviewers // []) | length > 0))] | length' 2>/dev/null || true)"
        [[ "$reviewer_rules" =~ ^[1-9][0-9]*$ ]] \
            || gate "正式版要求 $formal_environment 至少配置一条有 reviewer 的人工审批保护规则。"
        can_admins_bypass="$(gh api "repos/$EXPECTED_REPO/environments/$formal_environment" \
            --jq '.can_admins_bypass' 2>/dev/null || true)"
        [[ "$can_admins_bypass" == false ]] \
            || gate "正式版要求 $formal_environment 禁止 administrator bypass。"
        custom_branch_policies="$(gh api "repos/$EXPECTED_REPO/environments/$formal_environment" \
            --jq '.deployment_branch_policy.custom_branch_policies' 2>/dev/null || true)"
        [[ "$custom_branch_policies" == true ]] \
            || gate "正式版要求 $formal_environment 使用 selected branch/tag deployment policies。"
        formal_tag_policy="$(gh api "repos/$EXPECTED_REPO/environments/$formal_environment/deployment-branch-policies?per_page=100" \
            --jq '[.branch_policies[]?.name] | index("v*") != null' 2>/dev/null || true)"
        [[ "$formal_tag_policy" == true ]] \
            || gate "正式版要求 $formal_environment 允许 formal tag 模式 v*。"
    done

    # A protected Environment is the release authority once a tag exists, but
    # do not leave an accidental direct `git push origin vX.Y.Z` able to create
    # a permanent, unusable release tag. The active tag ruleset must require a
    # designated bypass actor for creation. GitHub still makes the final caller
    # authorization decision when the tag is pushed.
    creation_restriction=false
    ruleset_ids="$(gh api --paginate "repos/$EXPECTED_REPO/rulesets?per_page=100" \
        --jq '.[] | select(.target == "tag" and .enforcement == "active") | .id' 2>/dev/null || true)"
    while IFS= read -r ruleset_id; do
        [[ "$ruleset_id" =~ ^[1-9][0-9]*$ ]] || continue
        ruleset_controls="$(gh api "repos/$EXPECTED_REPO/rulesets/$ruleset_id" \
            --jq '[((.conditions.ref_name.include // []) | index("refs/tags/v*") != null), (([.rules[]?.type] | index("creation")) != null), ((.bypass_actors // []) | length > 0)] | all' \
            2>/dev/null || true)"
        if [[ "$ruleset_controls" == true ]]; then
            creation_restriction=true
            break
        fi
    done <<< "$ruleset_ids"
    [[ "$creation_restriction" == true ]] \
        || gate "正式版要求 active tag ruleset 覆盖 refs/tags/v*、启用 creation，并仅为指定发布主体配置 bypass。"
fi

# --- Plan -------------------------------------------------------------------
case "$KIND" in
    preview) channel="macOS 未签名预览版（Pre-release，不进入自动更新）" ;;
    stable) channel="四端正式安装包与更新清单（iOS 独立走 App Store 审核）" ;;
    platform) channel="Windows / Linux 数据预览版（Pre-release）" ;;
esac

echo ""
info "发布计划:"
echo "  仓库:     https://github.com/$EXPECTED_REPO"
echo "  渠道:     $channel"
echo "  上一 tag:  ${PREVIOUS:-<首次发布>}（仅版本命名，不代表公开 Release）"
echo "  新 tag:   $TAG"
echo "  提交:     $(git log -1 --format='%h %s' "$fetch_main")"
echo "  推送:     只推送 ${TAG}；不修改或提交任何文件"
for warning in "${WARNINGS[@]+"${WARNINGS[@]}"}"; do
    warn "$warning"
done

if [[ "$KIND" != "platform" ]]; then
    echo ""
    info "发布说明预览（只以公开 GitHub Release 为比较基线）:"
    python3 "$TOOL" notes --tag "$TAG" --ref "$fetch_main" | sed 's/^/  │ /'
fi

if [[ "$DRY_RUN" == true ]]; then
    echo ""
    if (( FAILED_GATES > 0 )); then
        warn "dry-run 完成：有 $FAILED_GATES 项门禁未满足，正式运行会被拒绝。"
    else
        success "dry-run 校验通过；没有创建或推送 tag。"
    fi
    exit 0
fi

if [[ "$ASSUME_YES" != true ]]; then
    [[ -t 0 ]] || die "非交互环境必须显式传入 --yes。"
    echo ""
    read -r -p "确认发布 $TAG 到 ${EXPECTED_REPO}？(y/n) " -n 1 reply
    echo
    [[ "$reply" =~ ^[Yy]$ ]] || { warn "已取消"; exit 0; }
fi

# Re-check after confirmation to narrow the window for a concurrent merge or tag.
[[ "$(remote_main_sha)" == "$fetch_main" ]] || die "确认期间 origin/main 已变化；请重新运行。"
remote_tag_exists "$TAG" && die "确认期间远端出现了 ${TAG}；已中止且不会覆盖。"

git tag "$TAG" "$fetch_main"
info "推送 $TAG 到 $EXPECTED_REPO..."
if ! git push "$REMOTE" "refs/tags/$TAG:refs/tags/$TAG"; then
    git tag -d "$TAG" >/dev/null
    die "推送失败；远端未创建 tag，本地 tag 已删除。"
fi

echo ""
success "已推送 ${TAG}，GitHub Actions 正在构建。"
echo "  构建进度: https://github.com/$EXPECTED_REPO/actions"
echo "  Release:  https://github.com/$EXPECTED_REPO/releases/tag/$TAG"
if [[ "$KIND" != "platform" ]]; then
    echo "  发布完成后，在下一个 PR 里运行 python3 $TOOL changelog --write 更新 CHANGELOG.md。"
fi
