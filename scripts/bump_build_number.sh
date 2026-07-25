#!/bin/bash
#
# bump_build_number.sh
# Usage: ./bump_build_number.sh path/to/build_number.h
#

set -euo pipefail

HEADER="${1:-}"

if [[ -z "$HEADER" ]]; then
    echo "Usage: $0 <header-file>" >&2
    exit 1
fi

mkdir -p "$(dirname "$HEADER")"

if [[ ! -f "$HEADER" ]]; then
    echo '#define BUILD_NUMBER 0' > "$HEADER"
fi

# Extract current number (robust even if file has guards/comments)
CURRENT=$(sed -n 's/.*#define[[:space:]]*BUILD_NUMBER[[:space:]]*\([0-9]*\).*/\1/p' "$HEADER" | tail -1)
CURRENT=${CURRENT:-0}

NEW=$((CURRENT + 1))

# Write a nice, proper C header
cat > "$HEADER" <<EOF
#ifndef BUILD_NUMBER_H
#define BUILD_NUMBER_H

#define BUILD_NUMBER $NEW

#endif /* BUILD_NUMBER_H */
EOF

echo "Build number incremented to $NEW"