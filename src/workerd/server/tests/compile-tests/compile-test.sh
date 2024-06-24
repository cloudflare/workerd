#!/bin/bash

set -o errexit
set -o nounset
set -o pipefail

# $1 -> workerd binary path
# $2 -> path to desired file to compile
# $3 -> port to curl
# $4 -> desired output

# Help Function
function show_help() {
  echo "
The Compile Test script is designed to aid in testing compiled workerd binaries.
It works by verifying that the result of a curl command on a running compiled binary matches an expected output.
usage: compile-test.sh [-d] [-h] <workerd command> <file-to-compile> <port-to-curl> <expected-output-file>
  options:
    -d print out where tmp files are created and do not delete them
    -h this help message

  Note: all flags must occur before arguments
"
}

while getopts "h" option; do
  case ${option} in
    h)
      show_help
      exit
      ;;
  esac
done

shift $(expr $OPTIND - 1)
WORKERD_BINARY=$1
CAPNP_SOURCE=$2
PORT=$3
EXPECTED=$4

CAPNP_BINARY=$(mktemp)

# Compile the app
$WORKERD_BINARY compile $CAPNP_SOURCE > $CAPNP_BINARY

# Run the app
$CAPNP_BINARY &
KILL=$!
OUTPUT=$(mktemp)
FIXED_OUTPUT=$(mktemp)
FIXED_EXPECTED=$(mktemp)

# Request output - and ensure the system is live
while ! curl localhost:$PORT -o $OUTPUT; do
  sleep .1
done;

# Compare the tests to the expected output
sed -e's/[[:space:]]*$//' $OUTPUT > $FIXED_OUTPUT
sed -e's/[[:space:]]*$//' $EXPECTED > $FIXED_EXPECTED
diff $FIXED_OUTPUT $FIXED_EXPECTED

# Clean up running workerd
kill -9 $KILL

# Clean up temp files
rm -f $CAPNP_BINARY
rm -f $OUTPUT
rm -f $FIXED_OUTPUT
rm -f $FIXED_EXPECTED
