# Profile-guided optimization

`--config=pgo-use` is the Linux release build (`release_linux` with `thin-lto`) optimized with
Clang's instrumentation-based PGO. The profile records which functions, branches and call sites a
training run exercised. The compiler uses it to decide what to inline, how to lay out code, and
which side of each branch to keep on the fall-through path. V8 is compiled in this build, so the
profile covers it too. The Rust code isn't covered: stable rustc can't share clang's profile
runtime.

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

4. Build with it:

   ```
   bazel build --config=pgo-use //src/workerd/server:workerd
   ```

Functions changed since the profile was trained no longer match it, and compile as if the profile
didn't cover them, so retrain after large changes to the hot code.
