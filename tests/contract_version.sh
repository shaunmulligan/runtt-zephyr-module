#!/usr/bin/env bash
# The module's own half of the contract assertion.
#
# The runtime's crates/runtt/tests/contract_version.rs cross-checks the document,
# the runtime's accepted major, the mock AND this module's Kconfig -- but only
# while they share a repository. Once the module stands alone as runtt-zephyr,
# that test can no longer see this file, and it skips.
#
# This is the other half, so nothing goes unasserted across the split: the module
# states which contract version it implements, and that claim is checked against
# the version it pins. Shell rather than Rust because this repo has no Rust in it.
#
#   ./tests/contract_version.sh                 (from the module root)
#   ./firmware/runtt/tests/contract_version.sh  (from the monorepo)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE="$(cd "$HERE/.." && pwd)"
KCONFIG="$MODULE/zephyr/Kconfig"

fail() { echo "FAIL: $*" >&2; exit 1; }

[[ -f "$KCONFIG" ]] || fail "no zephyr/Kconfig beside $MODULE"

# The version this module reports over `describe`.
DECLARED=$(awk '
  /^config RUNTT_CONTRACT_VERSION$/ { found = 1; next }
  found && /default / { gsub(/.*default *"?|"?$/, ""); print; exit }
' "$KCONFIG")
[[ -n "$DECLARED" ]] || fail "no default found for RUNTT_CONTRACT_VERSION in $KCONFIG"

# The version this module claims to implement. Standalone, that claim lives in
# CONTRACT_VERSION beside the module; in the monorepo, WIRE_CONTRACT.md is the
# authority and there is no separate file.
PINNED=""
SOURCE=""
if [[ -f "$MODULE/CONTRACT_VERSION" ]]; then
  PINNED=$(tr -d '[:space:]' < "$MODULE/CONTRACT_VERSION")
  SOURCE="CONTRACT_VERSION"
elif [[ -f "$MODULE/../../docs/WIRE_CONTRACT.md" ]]; then
  PINNED=$(sed -n 's/^\*\*Version \(.*\)\*\*$/\1/p' "$MODULE/../../docs/WIRE_CONTRACT.md" | head -1)
  SOURCE="docs/WIRE_CONTRACT.md"
else
  fail "found neither CONTRACT_VERSION nor docs/WIRE_CONTRACT.md to check against"
fi
[[ -n "$PINNED" ]] || fail "$SOURCE exists but states no version"

echo "  Kconfig default: $DECLARED"
echo "  $SOURCE: $PINNED"
[[ "$DECLARED" == "$PINNED" ]] \
  || fail "the module reports contract $DECLARED but $SOURCE says $PINNED. Whichever you changed, change the other."

# The interface string descriptors are contract too, and a board overlay that
# declares different ones produces a device the runtime cannot discover.
for iface in runtt-mgmt runtt-log; do
  grep -rq "$iface" "$MODULE/snippets" \
    || fail "no board overlay under snippets/ declares the $iface interface descriptor"
done
echo "  interface descriptors: runtt-mgmt and runtt-log declared"

echo "PASS: the module's contract claim is self-consistent."
