#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: scripts/release_and_commit.sh [patch|minor|major] [--dry-run] [-m <message>]

Bumps the Scribe release version, stages every repository change except vendor/,
and creates one local commit.

Defaults:
  bump part: patch
  commit message: Release Scribe <new-version>

Examples:
  scripts/release_and_commit.sh
  scripts/release_and_commit.sh minor
  scripts/release_and_commit.sh -m "Release Scribe v1.1.0"
  scripts/release_and_commit.sh --dry-run

The script updates the release version in:
  CMakeLists.txt
  include/scribe/scribe.h
  README.MD
  docs/MANUAL.md

It stages all tracked and untracked changes except paths under vendor/. It does
not push and does not create a tag. The GitHub release workflow creates the tag
after the pushed commit passes CI.
USAGE
}

read_cmake_version() {
    sed -nE 's/^project\(scribe VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt | tr -d '\r'
}

read_header_version() {
    sed -nE 's/^#define SCRIBE_VERSION "([^"]+)"/\1/p' include/scribe/scribe.h | tr -d '\r'
}

running_wsl_on_windows_checkout() {
    [ -n "${WSL_DISTRO_NAME:-}" ] && case "$repo_root" in /mnt/*) return 0 ;; *) return 1 ;; esac
}

bump_part="patch"
dry_run=0
commit_message=

while [ "$#" -gt 0 ]; do
    case "$1" in
        patch|minor|major)
            bump_part="$1"
            shift
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        -m|--message)
            if [ "$#" -lt 2 ]; then
                echo "$1 requires a commit message" >&2
                exit 2
            fi
            commit_message="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            echo "unexpected argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

if running_wsl_on_windows_checkout && [ "${SCRIBE_ALLOW_WSL_WINDOWS_CHECKOUT:-0}" != "1" ]; then
    cat >&2 <<'EOF'
Refusing to create a release commit from WSL on a /mnt/* Windows checkout.

In this repository shape, WSL Git may see CRLF-only changes across many files
that Windows Git does not see. Run this script from Git Bash/PowerShell instead,
or set SCRIBE_ALLOW_WSL_WINDOWS_CHECKOUT=1 if you have checked the status and
you intentionally want WSL Git to create the commit.
EOF
    exit 1
fi

version_files=(
    CMakeLists.txt
    include/scribe/scribe.h
    README.MD
    docs/MANUAL.md
)

cmake_version=$(read_cmake_version)
header_version=$(read_header_version)

if [ -z "$cmake_version" ]; then
    echo "Could not read project(scribe VERSION ...) from CMakeLists.txt" >&2
    exit 1
fi
if [ -z "$header_version" ]; then
    echo "Could not read SCRIBE_VERSION from include/scribe/scribe.h" >&2
    exit 1
fi
if [ "$cmake_version" != "$header_version" ]; then
    echo "Version mismatch: CMakeLists.txt has $cmake_version but SCRIBE_VERSION is $header_version" >&2
    exit 1
fi

IFS=. read -r major minor patch <<EOF
$cmake_version
EOF

case "$bump_part" in
    major)
        major=$((major + 1))
        minor=0
        patch=0
        ;;
    minor)
        minor=$((minor + 1))
        patch=0
        ;;
    patch)
        patch=$((patch + 1))
        ;;
esac

old_version="$cmake_version"
new_version="${major}.${minor}.${patch}"
if [ -z "$commit_message" ]; then
    commit_message="Release Scribe ${new_version}"
fi

echo "Scribe version: ${old_version} -> ${new_version}"
echo "Commit message: ${commit_message}"

if [ "$dry_run" -eq 1 ]; then
    echo "dry run: no files changed, no commit created"
    echo
    echo "Changes that would be committed, excluding vendor/:"
    git status --short -- . ':!vendor' ':!vendor/**'
    exit 0
fi

perl -0pi -e "s/project\\(scribe VERSION \\Q${old_version}\\E LANGUAGES C\\)/project(scribe VERSION ${new_version} LANGUAGES C)/" CMakeLists.txt
perl -0pi -e "s/#define SCRIBE_VERSION \"\\Q${old_version}\\E\"/#define SCRIBE_VERSION \"${new_version}\"/" include/scribe/scribe.h
perl -0pi -e "s/scribe version [0-9]+\\.[0-9]+\\.[0-9]+/scribe version ${new_version}/g" README.MD docs/MANUAL.md

updated_cmake_version=$(read_cmake_version)
updated_header_version=$(read_header_version)
if [ "$updated_cmake_version" != "$new_version" ] || [ "$updated_header_version" != "$new_version" ]; then
    echo "Version update failed: expected $new_version, got CMake=$updated_cmake_version header=$updated_header_version" >&2
    exit 1
fi

git add --all -- . ':!vendor' ':!vendor/**'

if git diff --cached --quiet; then
    echo "No staged changes to commit after excluding vendor/" >&2
    exit 1
fi

git commit -m "$commit_message"

echo "Created release commit for Scribe ${new_version}"
echo "vendor/ changes, if any, were left unstaged."
