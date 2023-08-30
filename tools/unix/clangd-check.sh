#!/bin/bash

# A script to scans workerd sources with clangd to look for issues
# that make affect language services.

output_dir=$(mktemp -d)
top_of_tree=$(git rev-parse --show-toplevel)
declare -a job_pids

# Guess at number of CPUs (could be probed, but portable requires work).
jobs_max=8

# Check bash version to see if `wait -n` is supported. This allows waiting on first of any
# process specified as an argument to wait to exit rather than waiting for all the processes
# specified to exit. (ie batch vs stream).
bash_major=$(echo $BASH_VERSION | sed -e 's/\..*/0000/')
bash_minor=$(echo $BASH_VERSION | sed -e 's/^[^.]*\.//' -e 's/\..*//')
bash_linear=$(($bash_major + $bash_minor))
if [ ${bash_linear} -ge 50001 ]; then
  wait_args="-n"
fi

cd "${top_of_tree}"
for i in $(find . -name '*.h' -o -name '*.c++'); do
    if [ ${#job_pids[*]} -eq ${jobs_max} ]; then
        # macOS inparticular is stuck on an older version of bash and does not support `wait -n`
        # here so child processes will run as batch waiting for all to complete there.
        wait ${wait_args} "${job_pids[@]}"
        for job_pid in "${job_pids[@]}"; do
          if ! kill -0 ${job_pid} 2>/dev/null ; then
            unset job_pids[${job_pid}]
          fi
        done
    fi
    j=${output_dir}/$(echo $i | sed  -e 's@^\./@@' -e s@/@_@g)
    echo -n Scanning $i =\> $j
    clangd --check=$i 1>$j 2>&1 &
    echo " [$!]"
    job_pids[$!]=$!
done

echo "Checking for broken includes."
grep "not found" ${output_dir}/*

cat <<EOF

You may also want to inspect the output files for other issues.

Please update compile_flags.txt to resolve any issues found.
EOF
