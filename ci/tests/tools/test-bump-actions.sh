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
  - uses: actions/held@3333333333333333333333333333333333333333 # v1
EOF

# actions/held is pinned by an external policy: it must survive every bump.
# The last entry deliberately has no trailing newline: `read` returns non-zero
# on it, and a loop that stops there would bump the held action.
printf '%s\n\n%s' '# comment lines and blanks are ignored' \
    'actions/held  allowed-actions policy pins the exact sha' \
    >"$work/repo/ci/tools/bump-actions.hold"

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
grep -q 'actions/held@3333333333333333333333333333333333333333 # v1$' \
    "$work/repo/.github/workflows/test.yml"
printf '%s\n' "$output" \
    | grep -q '^note: held actions/held at 333333333333 (v1): allowed-actions policy pins the exact sha$'
grep -q 'actions/example@aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa # v1.2.0' \
    "$work/repo/.github/workflows/test.yml"
grep -q 'actions/example@bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb # v2.1.0' \
    "$work/repo/.github/workflows/test.yml"

output="$(cd "$work/repo" && PATH="$work/bin:$PATH" bash ci/tools/bump-actions.sh)"
printf '%s\n' "$output" | grep -q '^ACTIONS_CHANGED=0$'

# Control: without the hold entry the same pin moves, so the assertion above
# is held-list behavior and not a stub that never resolves actions/held.
rm "$work/repo/ci/tools/bump-actions.hold"
output="$(cd "$work/repo" && PATH="$work/bin:$PATH" bash ci/tools/bump-actions.sh)"
printf '%s\n' "$output" | grep -q '^ACTIONS_CHANGED=1$'
grep -q 'actions/held@aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa # v1.2.0' \
    "$work/repo/.github/workflows/test.yml"

# A failing pin scan is fatal; it must not read as "nothing to bump".
mkdir -p "$work/badgrep"
cat >"$work/badgrep/grep" <<'EOF'
#!/usr/bin/env bash
echo "grep: .github/x.yml: Input/output error" >&2
exit 2
EOF
chmod +x "$work/badgrep/grep"
if output="$(cd "$work/repo" && PATH="$work/badgrep:$work/bin:$PATH" \
        bash ci/tools/bump-actions.sh 2>&1)"; then
    echo "FAIL: scan error reported success"; exit 1
fi
printf '%s\n' "$output" | grep -q '^FATAL: scanning .github/ for action pins failed (rc=2)$'

# No pins at all is a clean no-op (grep's "no match" exit 1 is not an error).
rm "$work/repo/.github/workflows/test.yml"
printf 'steps: []\n' >"$work/repo/.github/workflows/empty.yml"
output="$(cd "$work/repo" && PATH="$work/bin:$PATH" bash ci/tools/bump-actions.sh)"
printf '%s\n' "$output" | grep -q '^no sha-pinned actions found$'
printf '%s\n' "$output" | grep -q '^ACTIONS_CHANGED=0$'

echo "test-bump-actions: pass"
