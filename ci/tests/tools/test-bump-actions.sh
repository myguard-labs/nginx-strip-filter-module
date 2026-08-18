#!/usr/bin/env bash
# Regression tests for ci/tools/bump-actions.sh. Network calls are replaced by
# a deterministic gh stub so CI proves rewriting and exit behavior.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/repo/ci/tools" "$work/repo/.github/workflows" "$work/bin"
cp "$ROOT/ci/tools/bump-actions.sh" "$work/repo/ci/tools/"

cat >"$work/repo/.github/workflows/test.yml" <<'EOF'
steps:
  - uses: actions/example@1111111111111111111111111111111111111111 # v1
  - uses: actions/example@2222222222222222222222222222222222222222 # v2
EOF

cat >"$work/bin/gh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [ "$1 $2" = "release list" ]; then
    printf '%s\n' v2.1.0 v1.2.0
elif [ "$1" = api ] && [ "${*: -2:1}" = -q ]; then
    query="${*: -1}"
    if [ "$query" = .object.type ]; then
        echo commit
    elif [[ "$*" = *v1.2.0* ]]; then
        printf 'a%.0s' {1..40}; echo
    elif [[ "$*" = *v2.1.0* ]]; then
        printf 'b%.0s' {1..40}; echo
    else
        exit 1
    fi
else
    exit 1
fi
EOF
chmod +x "$work/bin/gh"

output="$(cd "$work/repo" && PATH="$work/bin:$PATH" bash ci/tools/bump-actions.sh)"
printf '%s\n' "$output" | grep -q '^ACTIONS_CHANGED=1$'
grep -q 'actions/example@aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa # v1.2.0' \
    "$work/repo/.github/workflows/test.yml"
grep -q 'actions/example@bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb # v2.1.0' \
    "$work/repo/.github/workflows/test.yml"

output="$(cd "$work/repo" && PATH="$work/bin:$PATH" bash ci/tools/bump-actions.sh)"
printf '%s\n' "$output" | grep -q '^ACTIONS_CHANGED=0$'

echo "test-bump-actions: pass"
