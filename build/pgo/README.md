# Profile-guided optimization

The Linux release builds, x64 and arm64 (`--config=release_linux`, which `bazelflare-release`
includes), are compiled with ThinLTO and Clang's instrumentation-based PGO. The profile records
which functions, branches and call sites a training run exercised, and the compiler uses it to
decide what to inline, how to lay out code, and which side of each branch to keep on the
fall-through path. V8 is compiled in this build, so the profile covers it too. So does it cover the
Rust code: rustc reads the same profile (`-Cprofile-use`) and compiles to bitcode
(`-Clinker-plugin-lto`, `--config=rust-lto`), so that the ThinLTO link optimizes the Rust and C++
code together, inlining across the cxx bridge.

`--config=release_linux-without-pgo` is the release build without ThinLTO or the profile, for
comparisons. The macOS release builds, arm64 and x64 (`--config=release_macos` and
`--config=release_macos_cross_x86_64`), use ThinLTO and the same profile, though ThinLTO leaves out
the Rust code, which rustc still optimizes with the profile (see [macOS](#macos));
`--config=release_macos-without-pgo` leaves both out. The Windows release build doesn't use the
profile yet.

## The profile

The profile isn't checked in. `build/pgo/profile.json` names it, an asset of the
[`pgo-profiles`](https://github.com/cloudflare/workerd/releases/tag/pgo-profiles) release, with its
hash, and Bazel downloads it into the `@workerd_pgo_profile` repository (`build/pgo/profile.bzl`).
rules_rust can't add an input through `extra_rustc_flag`, so `--config=pgo-use` adds the profile
to every Rustc action through `extra_rustc_inputs`, a flag that
`patches/rules_rust/0004-Add-a-flag-that-adds-inputs-to-every-Rustc-action.patch` adds. The same
patch replaces `${extra_rustc_inputs}` in the `-Cprofile-use` flag with the profile's path.

Release assets don't expire, but deleting one would break the release builds of every commit that
names it. [`release.yml`](../../.github/workflows/release.yml) runs the release builds on every
push to main, publishing only the first of each release version. So a missing profile, or a change
that breaks the PGO build, fails the merge that follows it. Its Linux and macOS builds also test
the PGO binary with the serving workload.

Clang matches the profile to the code in its own architecture-independent form, so the arm64 build
uses the profile trained on x64. Its target-specific code, which the x64 build doesn't have, gets
no profile data.

Functions changed since the profile was trained no longer match it, and compile as if the profile
didn't cover them, so retrain after changes that move the hot code, such as a V8 update.

## macOS

The macOS builds compile with Xcode's clang, whose LLVM is older than the toolchain's that writes
the profile. It reads indexed profiles up to version 12, and the profile is version 13, which
differs only in the hash that clang's frontend instrumentation uses by default. The profile comes
from IR instrumentation, which doesn't use that hash, so `//build/pgo:profile` gives Xcode's clang
a copy marked as version 12. Xcode's clang matches most of the functions that the profile covers:
in `src/workerd/io/io-context.c++`, 301 of the 394 it has data for; the others differ in their
control flow, as the compiler and headers aren't Linux's. Static functions in generated files don't
match at all, as their names include the output directory (`bazel-out/k8-opt/bin/...` on Linux).

The release builds link with lld, which the release workflow installs (`macos_use_lld`). By
default, Xcode's clang probes large stack frames through `__chkstk_darwin`, which only Xcode's
LLVM supports on arm64, so lld couldn't compile Xcode's arm64 bitcode. `--config=release_macos`
turns that off (`-fno-stack-check`) and turns on stack clash protection, which probes the same
frames inline, as LLVM supports on every target. The release workflow installs lld 21
(`macos_lld_formula`): Xcode's clang declares the coroutine intrinsic `llvm.coro.end` as returning
`i1`, and lld 22 and later, whose LLVM declares it as returning nothing, reject such bitcode.

The Rust code isn't compiled to bitcode. rustc still reads the profile, but most Rust functions
don't match it, as the names of those that involve the standard library's types include its hash,
which differs by target: of the 67 functions that `src/rust/kj` defines, 20 have names that the
profile knows.

## Training

The [Train the PGO profile](../../.github/workflows/pgo-train.yml) workflow trains a profile,
uploads it, and opens a pull request that points `profile.json` at it. Run it by hand from the
Actions tab. It runs `build/pgo/train.sh` and `build/pgo/upload.sh`, which work outside CI too, on
x64 Linux:

```
build/pgo/train.sh /tmp/pgo-profile --config=bazelflare
build/pgo/upload.sh /tmp/pgo-profile/workerd.profdata.zst
```

`train.sh`:

1. Builds `--config=pgo-instrument`, the release build with `-fprofile-generate` for C++ and
   `-Cprofile-generate` for Rust. The Rust code's counters go to the profile runtime that clang
   links, which stable rustc only allows with `RUSTC_BOOTSTRAP=1` (for `-Zno-profiler-runtime`),
   set for rustc actions alone through rules_rust's `extra_rustc_env`.
2. Runs every test that runs the workerd binary, in every variant but `@gc-stress`, under
   `build/pgo/profile-env.sh`. The wrapper sets `KJ_CLEAN_SHUTDOWN=1`, without which workerd exits
   through `_exit()`, skipping the profile runtime's exit handler that writes the profile, and it
   merges each test's raw profiles before Bazel collects them. The tests cover most of the
   runtime's APIs, but each starts a Worker and hands it a few events. The serving workload
   (`src/workerd/server/tests/pgo-workload`) covers the rest: `workerd serve` of a few Workers,
   each sent many requests by concurrent clients.
3. Merges the profiles, weighting the serving workload's (`PGO_WORKLOAD_WEIGHT`), and compresses
   the result.

To try a profile without uploading it, point the repository at a directory holding it:

```
mkdir -p /tmp/pgo-local && cp /tmp/pgo-profile/workerd.profdata /tmp/pgo-local/
echo 'exports_files(["workerd.profdata"])' > /tmp/pgo-local/BUILD.bazel
touch /tmp/pgo-local/REPO.bazel
bazel build --config=release_linux --override_repository=workerd_pgo_profile=/tmp/pgo-local \
    //src/workerd/server:workerd
```
