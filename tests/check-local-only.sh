#!/bin/sh
set -eu
# Each workflow entry is disallowed, including hidden files and symlinks.
root=${1:-$(dirname "$0")/..}
dir="$root/.github/workflows"
if [ -L "$root/.github" ] || [ -L "$dir" ]; then
    echo 'Workflow paths must not be symlinks' >&2
    exit 1
fi
if [ -d "$dir" ]; then
    for item in "$dir"/* "$dir"/.[!.]* "$dir"/..?*; do
        case "$item" in
            *.[yY][aA][mM][lL]|*.[yY][mM][lL])
                if [ -e "$item" ] || [ -L "$item" ]; then
                    printf 'Hosted workflow found: %s\n' "$item" >&2
                    exit 1
                fi ;;
        esac
    done
fi
