We use a combination of micro and macro benchmarks for performance testing workerd.

# Building benchmarks

Benchmarks should be built using `--config=benchmark` configuration, which is builds a release
binary with additional debug info.

To obtain most consistent results it is recommended to disable CPU frequency scaling
(use "performance" governor https://wiki.debian.org/CpuFrequencyScaling)

# Micro benchmarks

Micro benchmarks are defined using `wd_cc_benchmark` bazel macro. Use `bazel run --config=benchmark`
on a defined target to obtain benchmarking results.

See example in [bench-json.c++](../src/workerd/tests/bench-json.c++)

