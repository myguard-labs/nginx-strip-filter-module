#!/usr/bin/env bash
# Regression test for ci/tools/bump-versions.sh: a bump that rewrites a stamped
# .github/ file must leave every `# sync-sha:` stamp current, because the PR
# lane's Validation job runs `sync-stamp.sh --check` on the bump PR. Network
# work is replaced by stubs: compute-versions.sh is a no-op and bump-actions.sh
# rewrites one pin the way the real script does.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

repo="$work/repo"
mkdir -p "$repo/ci/tools" "$repo/.github/workflows" "$repo/.github/scripts"
cp "$ROOT/ci/tools/bump-versions.sh" "$ROOT/ci/tools/sync-stamp.sh" "$repo/ci/tools/"
printf 'NGINX_VERSION=1.0.0\n' >"$repo/.github/versions.env"

cat >"$repo/.github/scripts/compute-versions.sh" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF

cat >"$repo/ci/tools/bump-actions.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
[ "${1:-}" = "--dry-run" ] && exit 0
sed -i 's/@1111111111111111111111111111111111111111 # v1/@2222222222222222222222222222222222222222 # v1.1/' \
    .github/workflows/test.yml
EOF

cat >"$repo/.github/workflows/test.yml" <<'EOF'
name: test
on: push
jobs:
  t:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/example@1111111111111111111111111111111111111111 # v1
EOF

git -C "$repo" init -q
git -C "$repo" config user.email test@example.invalid
git -C "$repo" config user.name test
(cd "$repo" && bash ci/tools/sync-stamp.sh >/dev/null)
git -C "$repo" add -A
git -C "$repo" -c commit.gpgsign=false commit -qm init
(cd "$repo" && bash ci/tools/sync-stamp.sh --check >/dev/null)

# Dry run: nothing written, stamps untouched.
output="$(cd "$repo" && bash ci/tools/bump-versions.sh --dry-run)"
printf '%s\n' "$output" | grep -q '^CHANGED=0$'
[ -z "$(git -C "$repo" status --porcelain)" ]

# Real run: the pin moves AND its stamp is refreshed.
output="$(cd "$repo" && bash ci/tools/bump-versions.sh)"
printf '%s\n' "$output" | grep -q '^CHANGED=1$'
grep -q 'actions/example@2222222222222222222222222222222222222222 # v1.1' \
    "$repo/.github/workflows/test.yml"
(cd "$repo" && bash ci/tools/sync-stamp.sh --check >/dev/null)

echo "test-bump-versions-stamp: pass"
