#!/usr/bin/env bash
# release.sh — Create a GitHub release and trigger the Packages CI workflow.
#
# Pushing a v* tag starts .github/workflows/packages.yml, which builds all Linux
# distro packages and attaches them to the release when the workflow completes.
#
# Usage:
#   ./release.sh <version> [options]
#
# Examples:
#   ./release.sh 0.8.1 -m "0.8.1 — packaging fixes"
#   ./release.sh 0.8.2 --generate-notes
#   ./release.sh 0.8.2 --notes-file RELEASE_NOTES.md --dry-run
#
# Prerequisites:
#   - Packaging versions bumped and committed (see AGENTS.md release checklist)
#   - `make` doctests passing (unless --skip-build)
#   - `gh auth login` with repo scope
#   - Current branch synced with origin (master by default)

set -euo pipefail

REPO="${GITHUB_REPO:-ekollof/xepher}"
WORKFLOW_FILE="packages.yml"
DEFAULT_BRANCH="${RELEASE_BRANCH:-master}"

TAG_MESSAGE=""
RELEASE_TITLE=""
RELEASE_NOTES=""
NOTES_FILE=""
GENERATE_NOTES=0
DRAFT=0
SKIP_BUILD=0
SKIP_CHECKS=0
NO_WATCH=0
DRY_RUN=0

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

Options:
  -m, --message <text>     Annotated tag message (default: v<version>)
  -t, --title <text>       GitHub release title (default: v<version>)
  -n, --notes <text>       Release notes body
      --notes-file <path>  Read release notes from a file
      --generate-notes     Use GitHub auto-generated release notes
      --draft              Create a draft release
      --skip-build         Skip local `make` doctest run
      --skip-checks        Skip packaging-version and git-state checks
      --no-watch           Do not wait for the Packages workflow to finish
      --dry-run            Print actions without running them
  -h, --help               Show this help

Environment:
  GITHUB_REPO      Target repository (default: ekollof/xepher)
  RELEASE_BRANCH   Branch to release from (default: master)
EOF
}

log() {
    printf '==> %s\n' "$*"
}

die() {
    printf 'release.sh: %s\n' "$*" >&2
    exit 1
}

run() {
    if [ "${DRY_RUN}" -eq 1 ]; then
        printf '[dry-run]'; printf ' %q' "$@"; printf '\n'
    else
        "$@"
    fi
}

normalize_version() {
    local ver="${1#v}"
    if ! printf '%s' "${ver}" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([.-][0A-Za-z0-9.+~]+)?$'; then
        die "invalid version '${1}' (expected semver like 0.8.1)"
    fi
    printf '%s' "${ver}"
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

repo_root() {
    git rev-parse --show-toplevel 2>/dev/null || die "not inside a git repository"
}

packaging_version() {
    local file="$1"
    local pattern="$2"
    grep -E "${pattern}" "${file}" | head -n1 | sed -E "s/${pattern}/\1/"
}

check_packaging_versions() {
    local root ver
    root="$(repo_root)"
    ver="${1}"

    local arch_ver rpm_ver deb_ver fbsd_ver obsd_ver
    arch_ver=$(packaging_version "${root}/packaging/arch/PKGBUILD" '^pkgver=([0-9.]+)')
    rpm_ver=$(packaging_version "${root}/packaging/rpm/weechat-xmpp.spec" '^Version:[[:space:]]+([0-9.]+)')
    deb_ver=$(packaging_version "${root}/packaging/debian/changelog" '^xepher \(([0-9.]+)')
    fbsd_ver=$(packaging_version "${root}/packaging/freebsd/Makefile" '^PORTVERSION=([0-9.]+)')
    obsd_ver=$(packaging_version "${root}/packaging/openbsd/Makefile" '^V =[[:space:]]+([0-9.]+)')

    local mismatch=0
    for label in "arch:${arch_ver}" "rpm:${rpm_ver}" "debian:${deb_ver}" \
                 "freebsd:${fbsd_ver}" "openbsd:${obsd_ver}"; do
        local name="${label%%:*}"
        local value="${label#*:}"
        if [ "${value}" != "${ver}" ]; then
            printf '  %s: expected %s, found %s\n' "${name}" "${ver}" "${value}" >&2
            mismatch=1
        fi
    done

    if [ "${mismatch}" -ne 0 ]; then
        die "packaging files are not all bumped to ${ver} (see AGENTS.md release checklist)"
    fi
    log "packaging versions match ${ver}"
}

check_git_state() {
    local root branch
    root="$(repo_root)"
    branch="$(git -C "${root}" rev-parse --abbrev-ref HEAD)"

    if [ "${branch}" != "${DEFAULT_BRANCH}" ]; then
        die "on branch '${branch}', expected '${DEFAULT_BRANCH}' (override with RELEASE_BRANCH)"
    fi

    if [ -n "$(git -C "${root}" status --porcelain)" ]; then
        die "working tree is not clean; commit or stash changes before releasing"
    fi

    git -C "${root}" fetch origin "${DEFAULT_BRANCH}" >/dev/null 2>&1 || \
        die "failed to fetch origin/${DEFAULT_BRANCH}"

    local local_sha remote_sha
    local_sha="$(git -C "${root}" rev-parse HEAD)"
    remote_sha="$(git -C "${root}" rev-parse "origin/${DEFAULT_BRANCH}")"
    if [ "${local_sha}" != "${remote_sha}" ]; then
        die "local ${DEFAULT_BRANCH} (${local_sha:0:7}) differs from origin/${DEFAULT_BRANCH} (${remote_sha:0:7}); push or pull first"
    fi

    log "git state OK (${DEFAULT_BRANCH} @ ${local_sha:0:7}, clean)"
}

check_build() {
    local root
    root="$(repo_root)"
    log "running make (doctests)..."
    run make -C "${root}"
    log "build OK"
}

parse_args() {
    if [ "$#" -lt 1 ]; then
        usage
        exit 1
    fi

    VERSION="$(normalize_version "$1")"
    shift

    while [ "$#" -gt 0 ]; do
        case "$1" in
            -m|--message) TAG_MESSAGE="${2:?missing argument for $1}"; shift 2 ;;
            -t|--title) RELEASE_TITLE="${2:?missing argument for $1}"; shift 2 ;;
            -n|--notes) RELEASE_NOTES="${2:?missing argument for $1}"; shift 2 ;;
            --notes-file) NOTES_FILE="${2:?missing argument for $1}"; shift 2 ;;
            --generate-notes) GENERATE_NOTES=1; shift ;;
            --draft) DRAFT=1; shift ;;
            --skip-build) SKIP_BUILD=1; shift ;;
            --skip-checks) SKIP_CHECKS=1; shift ;;
            --no-watch) NO_WATCH=1; shift ;;
            --dry-run) DRY_RUN=1; shift ;;
            -h|--help) usage; exit 0 ;;
            *) die "unknown option: $1 (try --help)" ;;
        esac
    done

    if [ -n "${NOTES_FILE}" ] && [ -n "${RELEASE_NOTES}" ]; then
        die "use only one of --notes and --notes-file"
    fi
    if [ -n "${NOTES_FILE}" ]; then
        [ -f "${NOTES_FILE}" ] || die "notes file not found: ${NOTES_FILE}"
        RELEASE_NOTES="$(cat "${NOTES_FILE}")"
    fi
    if [ -z "${TAG_MESSAGE}" ]; then
        TAG_MESSAGE="v${VERSION}"
    fi
    if [ -z "${RELEASE_TITLE}" ]; then
        RELEASE_TITLE="v${VERSION}"
    fi
}

check_prerequisites() {
    require_cmd git
    require_cmd gh
    gh auth status >/dev/null 2>&1 || die "gh is not authenticated (run: gh auth login)"
}

tag_exists() {
    git rev-parse "$1" >/dev/null 2>&1
}

remote_tag_exists() {
    git ls-remote --exit-code --tags origin "$1" >/dev/null 2>&1
}

create_and_push_tag() {
    local tag="v${VERSION}"

    if tag_exists "${tag}"; then
        if [ "${DRY_RUN}" -eq 1 ]; then
            log "[dry-run] local tag ${tag} already exists (would fail without --dry-run)"
        else
            die "local tag ${tag} already exists"
        fi
    fi
    if remote_tag_exists "${tag}"; then
        if [ "${DRY_RUN}" -eq 1 ]; then
            log "[dry-run] remote tag ${tag} already exists (would fail without --dry-run)"
        else
            die "remote tag ${tag} already exists on origin"
        fi
    fi

    log "creating annotated tag ${tag}"
    run git tag -a "${tag}" -m "${TAG_MESSAGE}"
    log "pushing ${tag} to origin (triggers Packages workflow)"
    run git push origin "${tag}"
}

create_release() {
    local tag="v${VERSION}"
    local -a args=(release create "${tag}" --repo "${REPO}" --title "${RELEASE_TITLE}" --verify-tag)

    if [ "${DRAFT}" -eq 1 ]; then
        args+=(--draft)
    fi
    if [ "${GENERATE_NOTES}" -eq 1 ]; then
        args+=(--generate-notes)
        if [ -n "${RELEASE_NOTES}" ]; then
            args+=(--notes "${RELEASE_NOTES}")
        fi
    elif [ -n "${RELEASE_NOTES}" ]; then
        args+=(--notes "${RELEASE_NOTES}")
    else
        args+=(--notes-from-tag)
    fi

    log "creating GitHub release ${tag}"
    run gh "${args[@]}"
}

wait_for_packages_workflow() {
    local tag="v${VERSION}"
    local run_id

    if [ "${DRY_RUN}" -eq 1 ]; then
        log "[dry-run] would watch Packages workflow for ${tag}"
        return 0
    fi

    log "waiting for Packages workflow run..."
    for _ in $(seq 1 30); do
        run_id="$(gh run list \
            --repo "${REPO}" \
            --workflow "${WORKFLOW_FILE}" \
            --json databaseId,headBranch,status \
            --jq "map(select(.headBranch == \"${tag}\")) | .[0].databaseId" 2>/dev/null || true)"
        if [ -n "${run_id}" ] && [ "${run_id}" != "null" ]; then
            break
        fi
        sleep 2
    done

    if [ -z "${run_id}" ] || [ "${run_id}" = "null" ]; then
        die "could not find Packages workflow run for ${tag}; check: gh run list --repo ${REPO} --workflow ${WORKFLOW_FILE}"
    fi

    log "watching workflow run ${run_id}"
    gh run watch "${run_id}" --repo "${REPO}" --exit-status
    log "Packages workflow succeeded; release assets should be attached"
    gh run view "${run_id}" --repo "${REPO}" --web >/dev/null 2>&1 || true
}

main() {
    parse_args "$@"
    check_prerequisites

    if [ "${SKIP_CHECKS}" -eq 0 ]; then
        check_packaging_versions "${VERSION}"
        check_git_state
    fi
    if [ "${SKIP_BUILD}" -eq 0 ]; then
        check_build
    fi

    create_and_push_tag
    create_release

    log "release ${REPO} v${VERSION} published"
    printf '  https://github.com/%s/releases/tag/v%s\n' "${REPO}" "${VERSION}"

    if [ "${NO_WATCH}" -eq 0 ]; then
        wait_for_packages_workflow
    else
        log "skipping workflow watch (--no-watch)"
        log "monitor with: gh run watch --repo ${REPO} --workflow ${WORKFLOW_FILE}"
    fi
}

main "$@"