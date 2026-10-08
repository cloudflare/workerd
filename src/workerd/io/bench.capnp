# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xa7dcb95a0eecb745;

# Parameters and results of the `bench` event, which runs a Worker's `bench()` handlers under
# `workerd bench`. Production never sends this event.
#
# The JSON encoding of `BenchReport` (`workerd bench --format=json`) is not yet stable: fields may
# be renamed or removed.

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::bench");
$Cxx.allowCancellation;

struct BenchParams {
  caseFilter @0 :Text;
  # Glob on case names; empty or "*" runs all cases.

  quick @1 :Bool;
  # Short budgets, for checking that benchmarks run rather than measuring them.

  defaults @2 :CaseOptions;
  # Defaults for options that a case doesn't set.
}

struct CaseOptions {
  minTimeNs @0 :UInt64;
  # Sampling budget: calibration picks a batch size so that `samples` batches take about this long.

  warmupNs @1 :UInt64;
  # Time spent calling the function before calibration and sampling.

  samples @2 :UInt32;
  # The number of batches timed.

  batch @3 :UInt64;
  # Calls per batch; 0 calibrates.
}

struct BenchReport {
  environment @0 :Environment;
  groups @1 :List(Group);

  struct Environment {
    workerdVersion @0 :Text;
    buildMode @1 :Text;
    # "release", "debug", or "sanitizer".

    v8Version @2 :Text;
    cpuModel @3 :Text;
    governor @4 :Text;
    # The CPU frequency governor, if known (Linux).

    compatDate @5 :Text;
    # The `--compat-date` override, if any.

    allAutogates @6 :Bool;
    warnings @7 :List(Text);
    # Conditions that make results unreliable, such as a non-release build.
  }

  struct Group {
    # The cases registered by one `bench()` handler.

    name @0 :Text;
    # "service" or "service:entrypoint".

    error @1 :Text;
    # Set if `bench()` itself failed.

    cases @2 :List(Case);

    overhead @3 :Overhead;
    # The runner's own cost per call, which has been subtracted from the cases' samples.
  }

  struct Overhead {
    # The runner's cost per call, measured once per thread by timing empty cases the same way as
    # user cases, in the same units as `Case.wallNs` and `Case.cpuNs`. `workerd bench` runs every
    # group on one thread, so all groups in a report share one measurement.

    syncNs @0 :Float64;
    syncCpuNs @1 :Float64;
    # An empty function. Subtracted from cases whose function returns a value other than a promise.

    asyncNs @2 :Float64;
    asyncCpuNs @3 :Float64;
    # A function that returns a resolved promise. Subtracted from cases whose function returns a
    # promise.

    blackBoxNs @4 :Float64;
    # A call of `blackBox()`, including the call overhead. Not subtracted, since cases call it a
    # varying number of times.
  }

  struct Case {
    name @0 :Text;
    status @1 :Status;
    enum Status {
      ok @0;
      skipped @1;
      failed @2;
    }

    error @2 :Text;
    # Set if the case failed.

    iterationsPerSample @3 :UInt64;
    wallNs @4 :Metric;
    # Wall time per call.

    cpuNs @5 :Metric;
    # Thread CPU time per call. On Windows, the thread CPU clock advances only on scheduler ticks
    # (about 15ms), so unless batches are much longer than that, this is mostly noise.

    flags @6 :List(Flag);
    enum Flag {
      highVariance @0;
      # The coefficient of variation of the wall time exceeds 10%.

      atFloor @1;
      # The median wall time before subtracting the overhead is less than twice the overhead, so
      # the result is mostly the runner's own cost. V8 may have optimized away the work; pass
      # results through `blackBox()`.
    }
  }

  struct Metric {
    # Statistics over the samples of one metric. Each sample is a batch's total divided by its
    # number of calls. Statistics come from an HDR histogram with 3 significant figures, so they
    # are accurate to 0.1%; `samples` are exact.

    samples @0 :List(Float64);
    mean @1 :Float64;
    meanLow @2 :Float64;
    meanHigh @3 :Float64;
    # 95% confidence interval on the mean (Student's t).

    median @4 :Float64;
    medianLow @5 :Float64;
    medianHigh @6 :Float64;
    # 95% confidence interval on the median (exact binomial).

    p75 @7 :Float64;
    p99 @8 :Float64;
    min @9 :Float64;
    max @10 :Float64;
    stddev @11 :Float64;
    skewness @12 :Float64;
    kurtosis @13 :Float64;
  }
}
