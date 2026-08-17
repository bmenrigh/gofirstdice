#!/usr/bin/env bash

set -u
set -o pipefail

script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
column_search=${COLUMN_SEARCH:-"$script_directory/column_search"}
template_directory=${1:-"$script_directory/expanded_4d30_templates"}
solutions_file=${2:-"$script_directory/found_solutions_5d30.txt"}
done_directory="$template_directory/done"

jobs=${JOBS:-78125}
progress_seconds=${PROGRESS_SECONDS:-5}
threads=${THREADS:-24}

trap 'exit 130' INT
trap 'exit 143' TERM

if [[ ! -x "$column_search" ]]; then
    echo "column_search is not executable: $column_search" >&2
    exit 1
fi
if [[ ! -d "$template_directory" ]]; then
    echo "Template directory does not exist: $template_directory" >&2
    exit 1
fi
if ! mkdir -p -- "$done_directory"; then
    echo "Unable to create done directory: $done_directory" >&2
    exit 1
fi

shopt -s nullglob
templates=("$template_directory"/*.template)

if ((${#templates[@]} == 0)); then
    echo "No templates remain in $template_directory"
    exit 0
fi

mapfile -d '' -t templates < <(
    printf '%s\0' "${templates[@]}" | sort -zR
)

for template in "${templates[@]}"; do
    destination="$done_directory/${template##*/}"

    if [[ -e "$destination" || -L "$destination" ]]; then
        echo "Refusing to overwrite completed template: $destination" >&2
        exit 1
    fi

    echo "Searching ${template##*/}"
    status=0
    "$column_search" \
        --random-order \
        --jobs "$jobs" \
        -p "$progress_seconds" \
        -t "$threads" \
        --solutions-file "$solutions_file" \
        --template "$template" || status=$?

    if ((status != 0)); then
        echo "Search failed with status $status; template was not moved: $template" >&2
        exit "$status"
    fi

    if ! mv -- "$template" "$destination"; then
        echo "Search completed, but the template could not be moved: $template" >&2
        exit 1
    fi
    echo "Completed ${template##*/}"
done
