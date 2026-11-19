#!/usr/bin/env bash
# Static scan: Linux/glibc-only host APIs may appear only under platform/
# (docs/ARCHITECTURE.md, "Portability rules", item 4). Comments are
# ignored; the scan covers runtime/ and tests/ (everything except platform/).
set -euo pipefail
cd "$(dirname "$0")/.."

patterns=(
    '/proc/' '/sys/' 'memfd_create' 'mremap' 'MAP_FIXED_NOREPLACE' 'MAP_32BIT'
    'epoll_[a-z_]*' 'sys/epoll\.h' 'sys/sysinfo\.h' 'sysinfo[[:space:]]*\('
    'sched_getcpu' 'sched_setaffinity' 'pthread_setaffinity_np' 'pthread_getattr_np'
    'prctl[[:space:]]*\(' 'ptrace[[:space:]]*\(' 'process_vm_[a-z]*' 'userfaultfd'
    'getauxval' 'sys/auxv\.h' 'sys/prctl\.h' 'linux/[a-z_/]*\.h' 'gnu/libc-version\.h'
    'gnu_get_libc_version' '__GLIBC__' '/etc/' 'secure_getenv' 'program_invocation_name'
    'sys/inotify\.h' 'signalfd' 'timerfd_[a-z]*' 'eventfd' 'O_TMPFILE' 'getrandom[[:space:]]*\('
    'xlocale\.h' 'malloc_usable_size' 'mallinfo' '__libc_[a-z_]*' 'MAP_GROWSDOWN'
    # setjmp/longjmp variants that save the signal mask (non-portable behaviour).
    '(^|[^_a-z])setjmp[[:space:]]*\(' '(^|[^_a-z])longjmp[[:space:]]*\('
    'sigsetjmp[[:space:]]*\([^,]*,[[:space:]]*[1-9a-zA-Z_]'
)
joined=$(IFS='|'; echo "${patterns[*]}")

status=0
while IFS= read -r file; do
    # Strip // and /* */ comments (single-line approximation) before matching.
    hits=$(sed -E 's://.*$::; s:/\*([^*]|\*[^/])*\*/::g' "$file" |
        grep -nE "$joined" || true)
    if [ -n "$hits" ]; then
        status=1
        printf 'HOST_API_OUTSIDE_PLATFORM %s\n%s\n' "$file" "$hits"
    fi
done < <(find runtime tests -type f \( -name '*.c' -o -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) | sort)

if [ "$status" -ne 0 ]; then
    printf 'Linux/glibc-only API used outside platform/.\n' >&2
    exit 1
fi

printf 'HOST_API_SCAN_CLEAN\n'
