#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
usage: scripts/push-versioned.sh [patch|minor|major] [--dry-run] [--no-push] [-- <git-push-args>...]

Bumps the Scribe project version, commits the version files, then runs git push.

Defaults:
  bump part: patch
  push command: git push

Examples:
  scripts/push-versioned.sh
  scripts/push-versioned.sh minor
  scripts/push-versioned.sh patch -- origin main
  scripts/push-versioned.sh --dry-run
  scripts/push-versioned.sh --no-push patch

The script updates:
  CMakeLists.txt
  include/scribe/scribe.h
  README.MD
  docs/MANUAL.md

It commits with:
  Bump Scribe version to <new-version>

It does not create a local tag. The GitHub release workflow creates v<version>
after the pushed commit passes the quality gate.
USAGE
}

read_cmake_version() {
    sed -nE 's/^project\(scribe VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt
}

read_header_version() {
    sed -nE 's/^#define SCRIBE_VERSION "([^"]+)"/\1/p' include/scribe/scribe.h
}

require_clean_version_files() {
    local files=("$@")

    if ! git diff --quiet -- "${files[@]}" || ! git diff --cached --quiet -- "${files[@]}"; then
        echo "Refusing to bump version because version-managed files already have local changes:" >&2
        git status --short -- "${files[@]}" >&2
        echo >&2
        echo "Commit, stash, or discard those changes first." >&2
        exit 1
    fi
}

bump_part="patch"
dry_run=0
no_push=0
push_args=()

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
        --no-push)
            no_push=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            push_args=("$@")
            break
            ;;
        -*)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            push_args+=("$1")
            shift
            ;;
    esac
done

repo_root=$(git rev-parse --show-toplevel)
cd "$repo_root"

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

echo "Scribe version: ${old_version} -> ${new_version}"

if [ "$dry_run" -eq 1 ]; then
    echo "dry run: no files changed, no commit created, no push performed"
    exit 0
fi

require_clean_version_files "${version_files[@]}"

perl -0pi -e "s/project\\(scribe VERSION \\Q${old_version}\\E LANGUAGES C\\)/project(scribe VERSION ${new_version} LANGUAGES C)/" CMakeLists.txt
perl -0pi -e "s/#define SCRIBE_VERSION \"\\Q${old_version}\\E\"/#define SCRIBE_VERSION \"${new_version}\"/" include/scribe/scribe.h
perl -0pi -e "s/scribe version \\Q${old_version}\\E/scribe version ${new_version}/g" README.MD docs/MANUAL.md

updated_cmake_version=$(read_cmake_version)
updated_header_version=$(read_header_version)
if [ "$updated_cmake_version" != "$new_version" ] || [ "$updated_header_version" != "$new_version" ]; then
    echo "Version update failed: expected $new_version, got CMake=$updated_cmake_version header=$updated_header_version" >&2
    exit 1
fi

git add -- "${version_files[@]}"
git commit -m "Bump Scribe version to ${new_version}"

if [ "$no_push" -eq 1 ]; then
    echo "version bump committed; --no-push requested"
    exit 0
fi

if [ "${#push_args[@]}" -eq 0 ]; then
    git push
else
    git push "${push_args[@]}"
fi
