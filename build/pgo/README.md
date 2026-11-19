# Profile-guided optimization

`--config=pgo-use` is the Linux release build (`release_linux` with `thin-lto`) optimized with
Clang's instrumentation-based PGO. The profile records which functions, branches and call sites a
training run exercised. The compiler uses it to decide what to inline, how to lay out code, and
which side of each branch to keep on the fall-through path. V8 is compiled in this build, so the
profile covers it too. `--config=pgo-use` applies the profile to the C++ code only;
`--config=pgo-use-rust` applies it to the Rust code as well, and `--config=rust-lto` adds the Rust
code to the ThinLTO link (see step 4).

The instrumented build counts the Rust code too. Its counters go to the profile runtime that clang
links, which stable rustc only allows with `RUSTC_BOOTSTRAP=1` (for `-Zno-profiler-runtime`), set
for rustc actions alone through rules_rust's `extra_rustc_env`.

The profile isn't checked in. To make one:

1. Build the instrumented binary:

   ```
   bazel build --config=pgo-instrument //src/workerd/server:workerd
   ```

2. Run it on representative Workers with `LLVM_PROFILE_FILE` set, then stop it with SIGTERM.
   `workerd serve` exits through `_exit()`, which skips the profile runtime's exit handler, unless
   `KJ_CLEAN_SHUTDOWN=1` is set:

   ```
   KJ_CLEAN_SHUTDOWN=1 LLVM_PROFILE_FILE=/tmp/pgo/workerd-%p.profraw \
       bazel-bin/src/workerd/server/workerd serve config.capnp
   ```

3. Merge the raw profiles with the `llvm-profdata` of the toolchain's LLVM release, which is in
   `$(bazel info output_base)/external/toolchains_llvm++llvm+llvm_linux_x86_64_llvm/bin`
   (run `bazel fetch --repo=@@toolchains_llvm++llvm+llvm_linux_x86_64_llvm` first):

   ```
   llvm-profdata merge -o build/pgo/workerd.profdata /tmp/pgo/*.profraw
   ```

4. Build with it. rustc can't read the profile as a Bazel input, so copy it to a path outside
   `/tmp` named by its hash, and pass that path:

   ```
   p=$HOME/pgo-profiles/workerd-$(sha256sum build/pgo/workerd.profdata | cut -c1-16).profdata
   mkdir -p $HOME/pgo-profiles && cp build/pgo/workerd.profdata $p
   bazel build --config=pgo-use-rust --config=rust-lto \
       --@rules_rust//:extra_rustc_flag=-Cprofile-use=$p //src/workerd/server:workerd
   ```

   `pgo-use-rust` runs the Rust compile actions in the local sandbox, since remote executors don't
   have the file. `--config=pgo-use` alone skips the Rust part and builds entirely remotely.

Functions changed since the profile was trained no longer match it, and compile as if the profile
didn't cover them, so retrain after large changes to the hot code.

## BOLT

BOLT rearranges the linked binary's code using a profile of that binary, which the compiler's
profile can't fully do: it places basic blocks and functions across the whole binary, and moves
cold code out of the hot pages. On top of the PGO build it gave about 2-3% more requests per
second on hello-world benchmarks, with about 10% fewer front-end stall cycles. BOLT's own
instrumentation is used, so this works on machines without LBR. `llvm-bolt` and `merge-fdata` are
in the same toolchain directory as `llvm-profdata`.

1. Add `--config=bolt` to the step 4 build, which keeps the relocations BOLT needs.

2. Instrument the binary. V8 finds its embedded builtins by offsets into the embedded blob, so
   they have to stay where they are:

   ```
   llvm-bolt workerd -skip-funcs='Builtins_.*' -instrument -o workerd.inst \
       -instrumentation-file=/tmp/bolt/workerd.fdata -instrumentation-file-append-pid
   ```

3. Run `workerd.inst` on the same Workers as step 2 above, with `KJ_CLEAN_SHUTDOWN=1`, and stop it
   with SIGTERM. Each process writes `/tmp/bolt/workerd.fdata.<pid>.fdata` as it exits.

4. Merge the profiles and lay the binary out:

   ```
   merge-fdata /tmp/bolt/workerd.fdata.*.fdata > workerd.fdata
   llvm-bolt workerd -skip-funcs='Builtins_.*' -o workerd.bolt -data=workerd.fdata \
       -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions -split-all-cold \
       -split-eh -icf=1 -use-gnu-stack
   ```
