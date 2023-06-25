#!/bin/bash

# A script to scans workerd sources with clangd to look for issues
# that make affect language services.

output_dir=$(mktemp -d)
top_of_tree=$(git rev-parse --show-toplevel)

cd "${top_of_tree}"
for i in $(find . -name '*.h' -o -name '*.c++'); do
    j=${output_dir}/$(echo $i | sed  -e 's@^\./@@' -e s@/@_@g)
    echo Scanning $i =\> $j
    clangd --check=$i 1>$j 2>&1
done
