# V8 Updates

To update the version of V8 used by workerd, the steps are:

1. Check <https://chromiumdash.appspot.com/> and identify the latest version of V8 used by the beta versions of Chrome beta.

2. Install `depot_tools` if it is not already present on your machine.

   <https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html#_setting_up>

3. Fetch a local copy of V8:

   ```sh
   mkdir v8
   cd v8
   fetch v8
   ```

   You should probably put this outside of your workerd repo to avoid confusing Bazel.

4. Sync the local copy of V8 to the version used by workerd.

   First, find workerd's current version of V8 in `build/deps/v8.MODULE.bazel`. We will call this `<old_version>`.

   Then sync your fetched version v8 based on the tag.

   ```sh
   cd <path_to_v8>/v8
   git checkout <old_version>
   gclient sync
   ```

5. Create a V8 branch for workerd's V8 patches in your local copy of V8.

   ```sh
   git checkout -b workerd-patches
   git am <path_to_workerd>/patches/v8/*
   ```

6. Rebase the workerd V8 changes onto the new version of V8. For example, assuming
   we are updating to `<new_version>`, the command would be:

   ```sh
   git rebase --onto <new_version> <old_version>
   ```

   There is usually some minor patch editing required during a rebase.

   Ideally at this stage, you should be able to build and test the local V8 with the
   patches applied. See the V8 [Testing](https://v8.dev/docs/test) page.

7. Re-generate workerd's V8 patches.

   ```sh
   git format-patch --full-index -k --no-signature --no-stat --zero-commit <new_version>
   ```

8. Remove the existing patches from `<path_to_workerd>/patches/v8` and copy over the latest generated patches
from the V8 directory.

9. Update the `VERSION` for V8 in `build/deps/v8.MODULE.bazel`.

    The list of patches should be refreshed if new patches are being added or existing
    patches are being removed.

    `INTEGRITY` needs to be updated to the new value. You can get the new value in
    bazel's preferred format just by looking into the mismatch error while trying to compile
    workerd using the newer V8 version or by running
    `openssl dgst -sha256 -binary <tarball_filename> | openssl base64 -A`
    where `<tarball_filename>` is the file available at
    `https://github.com/v8/v8/archive/refs/tags/<new_version>.tar.gz`

10. Update V8's dependencies in `v8.MODULE.bazel` and `deps.MODULE.bazel`.

    You can find the commit versions for V8's dependencies under `<path_to_v8>/DEPS`.

    These currently include `perfetto`, `com_googlesource_chromium_icu` and `simdutf`.
    Note that V8 depends on `perfetto` and `simdutf` via chromium so you can't trivially
    figure out what version of the github code V8 depends on. Instead, it should be safe
    to just bump these dependencies to the latest version on github.

11. Check workerd's tests pass with the updated V8.

     ```sh
     bazel test //...
     ```

12. Commit your workerd changes and push them for review.

## Update Helper

`ci/v8_update.py` provides an alternative workflow for automating the
mechanical parts of steps 1 through 10. It has not been thoroughly tested, so review its
changes carefully and fall back to the manual workflow above if it fails.

Check whether Chrome Beta uses a newer V8 version:

```sh
python3 ci/v8_update.py check-update
```

The command prints `<current_version> -> <new_version>` and exits with status 1 when an
update is available. It prints nothing and exits successfully when workerd is current.
Pass `--machine-readable` to print only the new version.

Run the update using the reported version:

```sh
python3 ci/v8_update.py update <new_version>
```

The helper creates a shallow V8 checkout at `/tmp/workerd-v8/v8`, applies and
rebases the patches, regenerates `patches/v8/*.patch`, and updates:

- V8's `VERSION`, `INTEGRITY`, patch list, and ICU commit in
  `build/deps/v8.MODULE.bazel`;
- the `dragonbox`, `fast_float`, `fp16`, and `highway` commits in
  `build/deps/deps.jsonc`.

If the rebase stops because of conflicts, resolve and continue it in the temporary
checkout:

```sh
cd /tmp/workerd-v8/v8
git status
# Resolve the conflicts and stage the files.
git rebase --continue
```

Repeat until the rebase completes. Patch conflicts require human review to ensure that
workerd-specific behavior is preserved. Then return to the workerd checkout and finish
the update:

```sh
python3 ci/v8_update.py finish <new_version>
```

Do not run `finish` until the rebase has completed. It regenerates the patches and
updates the pins listed above.

The helper does not run `update-deps.py`. Compare the old and new V8 `DEPS` files for
changes to `dragonbox`, `fast_float`, `fp16`, `highway`, `perfetto`, and `simdutf`, then
run the dependency updater for each changed revision:

```sh
python3 build/deps/update-deps.py <dependency>
```

Complete steps 11 and 12 from the manual workflow after reviewing the generated changes.
