#!/usr/bin/env bash

# This is a helper script various binary depdenecies for the windows-2022 and windows-2022/base
# Packer templates.

set -euo pipefail

# Download relative to this script.
ABS_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Remove any old Packer configuration for these deps.
rm "$ABS_SCRIPT_DIR/base/deps.auto.pkrvars.hcl" 2> /dev/null || true
rm "$ABS_SCRIPT_DIR/deps.auto.pkrvars.hcl" 2> /dev/null || true

# Download.
(cd "$ABS_SCRIPT_DIR" && aria2c --check-integrity --input-file deps.aria2-input)

# Configure windows-2022/base
echo "Writing base/deps.auto.pkrvars.hcl"
cat << EOF > "$ABS_SCRIPT_DIR/base/deps.auto.pkrvars.hcl"
virtio_win_iso = "$ABS_SCRIPT_DIR/base/deps/virtio-win.iso"
EOF

# Configure windows-2022
echo "Writing deps.auto.pkrvars.hcl"
cat << EOF > "$ABS_SCRIPT_DIR/deps.auto.pkrvars.hcl"
deps_dir = "$ABS_SCRIPT_DIR/deps"
EOF

printf "Packer templates now configured. Build with \`packer build $ABS_SCRIPT_DIR/base && packer build $ABS_SCRIPT_DIR\`.\n"
