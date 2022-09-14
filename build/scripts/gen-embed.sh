#!/bin/bash
# Usage: gen-embed.sh <OUT_HEADER> <OUT_ASSEMBLY> [<INPUT_FILE1> <INPUT_NAME1> ...]

HEADER=$1
shift
ASSEMBLY=$1
shift
SRCS=( $@ )

generate_asm() {
  SHA=$(sha256sum ${2})
  cat >>$ASSEMBLY << __EOF__
// ${SHA}
.global ${1}_begin
.type ${1}_begin, @object
.align 8

${1}_begin:
.incbin "$2"

.global ${1}_end
.type ${1}_end, @object
${1}_end:

__EOF__
}

embed() {
  INPUT=$1
  NAME=$2

  # Construct the symbol name. We strip "src/" or "tmp/" from the front and use a different prefix.
  SYMBOL_NAME=EMBED_$(tr ./- ___ <<< "${NAME#*/}")

  # Write header declarations.
  cat >>$HEADER << __EOF__
extern const ::kj::byte ${SYMBOL_NAME}_begin;
extern const ::kj::byte ${SYMBOL_NAME}_end;
#define ${SYMBOL_NAME} (::kj::ArrayPtr<const ::kj::byte>( \\
    &${SYMBOL_NAME}_begin, &${SYMBOL_NAME}_end))

__EOF__

  # Write asm.
  generate_asm $SYMBOL_NAME "$INPUT"
}


INCLUDE_GUARD=$(tr a-z./- A-Z___ <<< "$HEADER")_

cat >$HEADER << __EOF__
#ifndef $INCLUDE_GUARD
#define $INCLUDE_GUARD
#include <kj/common.h>

__EOF__

cat >$ASSEMBLY << __EOF__
.section ".rodata"
__EOF__


n=${#SRCS[@]}
for ((i=0; i<n; i+=2));
do
  SRC=${SRCS[i]}
  NAME=${SRCS[i+1]}
  embed $SRC $NAME
done

cat >>$HEADER << __EOF__
#endif  // $INCLUDE_GUARD
__EOF__
