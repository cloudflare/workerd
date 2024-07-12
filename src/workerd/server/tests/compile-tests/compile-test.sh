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
EXPECTED=$3

CAPNP_BINARY=$(mktemp)
PORT_FILE=$(mktemp)

# Compile the app
$WORKERD_BINARY compile $CAPNP_SOURCE > $CAPNP_BINARY

# Run the app
$CAPNP_BINARY -shttp=localhost:0 --control-fd=1 > $PORT_FILE &
KILL=$!

# Make intermediate files before trying to wait on the bindings
OUTPUT=$(mktemp)
FIXED_OUTPUT=$(mktemp)
FIXED_EXPECTED=$(mktemp)

# Wait on the port bindings to occur
while ! grep \"socket\"\:\"http\" $PORT_FILE; do
  sleep .1
done

# Identify the port chosen by the binary
PORT=`grep \"socket\"\:\"http\" $PORT_FILE | sed 's/^.*\"port\"://g' | sed 's/\}//g' |head -n 1`

# Request output
curl localhost:$PORT -o $OUTPUT

# Compare the tests to the expected output
sed -e's/[[:space:]]*$//' $OUTPUT > $FIXED_OUTPUT
sed -e's/[[:space:]]*$//' $EXPECTED > $FIXED_EXPECTED
diff $FIXED_OUTPUT $FIXED_EXPECTED

# Clean up running workerd
kill -9 $KILL

# Clean up temp files
rm -f $CAPNP_BINARY
rm -f $PORT_FILE
rm -f $OUTPUT
rm -f $FIXED_OUTPUT
rm -f $FIXED_EXPECTED
