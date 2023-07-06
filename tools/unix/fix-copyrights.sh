#!/bin/sh

# Copyright (c) ${date_range} Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

SCRIPT_DIR=$(realpath $(dirname "$0"))/../..
CURRENT_YEAR=$(date +"%Y")

function copyright() {
  local source_file="$1"
  local date_range="$2"
  local wip_file=$(mktemp -t $(basename "${source_file}"))
  cat <<EOF > "${wip_file}"
// Copyright (c) ${date_range} Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

EOF
  cat "${source_file}" >> "${wip_file}"
  mv "${wip_file}" "${source_file}"
}

cd "${SCRIPT_DIR}"
for source_file in $(find . -name '*.c++' -o -name '*.h' -o -name '*.ts' -o -name '*.js') ; do
  if grep -Liq copyright "${source_file}" ; then
    continue
  fi
  if [[ "${source_file}" = *"node"* ]]; then
    continue
  fi
  commit=$(git log --oneline --format="%as %h %s" --reverse ${source_file} | head -1)
  commit_date=$(echo ${commit}| sed -e 's/ .*//')
  commit_year=$(echo ${commit_date}| sed -e 's/-.*//')
  if [ "${commit_date}" = "2022-09-13" -o "${commit_date}" = "2022-09-19" ]; then
    copyright "${source_file}" "2017-2023"
  elif [ "${commit_year}" = "${CURRENT_YEAR}" ]; then
    copyright "${source_file}" "${CURRENT_YEAR}"
  else
    copyright "${source_file}" "${commit_year}-${CURRENT_YEAR}"
  fi
done
