#!/usr/bin/env bash
# Checks the layout of every tracked source file (CONTRIBUTING.md, "Code style"); CI runs it on every push.
#   C, C++, Java: clang-format 19.1.7 with .clang-format (CLANG_FORMAT=..., else build/fmt-venv or the PATH)
#   Python:       tools/format_python.py --check
#   shell, CMake: at most 120 columns; shell scripts parse with bash -n
# To fix: clang-format -i FILE..., tools/format_python.py FILE...
set -euo pipefail
cd "$(dirname "$0")/.."
status=0

clang_format=${CLANG_FORMAT:-}
if [ -z "$clang_format" ]; then
    if [ -x build/fmt-venv/bin/clang-format ]; then
        clang_format=build/fmt-venv/bin/clang-format
    else
        clang_format=clang-format
    fi
fi

if ! "$clang_format" --version | grep -q 'version 19\.1\.7'; then
    echo "check_format: needs clang-format 19.1.7 (python3 -m venv build/fmt-venv &&" \
        "build/fmt-venv/bin/pip install clang-format==19.1.7)" >&2
    exit 1
fi

git ls-files -z -- '*.c' '*.cpp' '*.h' '*.java' ':!third_party' |
    xargs -0 "$clang_format" --dry-run --Werror || status=1

git ls-files -z -- '*.py' ':!third_party' | xargs -0 python3 tools/format_python.py --check || status=1

while IFS= read -r -d '' file; do
    awk -v f="$file" 'length > 120 { printf "%s:%d: longer than 120 columns\n", f, NR; bad = 1 } END { exit bad }' \
        "$file" >&2 || status=1
    case "$file" in
        CMakeLists.txt | */CMakeLists.txt) ;;
        *) bash -n "$file" || status=1 ;;
    esac
done < <(git ls-files -z -- '*.sh' 'CMakeLists.txt' 'tools/dist/sacred-gold' ':!third_party')

[ "$status" = 0 ] && echo "check_format: clean"
exit "$status"
