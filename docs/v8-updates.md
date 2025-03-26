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

4. Sync the local copy of V8 to the version used by workerd.

   First, find workerd's current version of V8 in `v8.bzl`. We will call this `<old_version>`.

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

9. Update the `VERSION` for V8 in `v8.bzl`.

    The list of patches should be refreshed if new patches are being added or existing
    patches are being removed.

    `INTEGRITY` needs to be updated to the new value. You can get the new value in
    bazel's preferred format just by looking into the mismatch error while trying to compile
    workerd using the newer V8 version or by running
    `openssl dgst -sha256 -binary <tarball_filename> | openssl base64 -A`

10. Update V8's dependencies in `v8.bzl` and `WORKSPACE`.

    You can find the commit versions for V8's dependencies under `<path_to_v8>/DEPS`.

    These currently include `zlib` and `com_googlesource_chromium_icu`.
    Typically you'll get a build failure if the projects are out of sync. Copy the
    commit versions from `v8/DEPS` to the `v8.bzl` or `WORKSPACE` file.

11. Check workerd's tests pass with the updated V8.

     ```sh
     bazel test //...
     ```

12. Commit your workerd changes and push them for review.
