# V8 Updates

To update the version of V8 used by workerd, the steps are:

1. Check https://omahaproxy.appspot.com/ and identify the latest version of V8 used by the beta versions of Chrome beta.

2. Install depot_tools if it is not already present on your machine.

   https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html#_setting_up

3. Fetch V8

   ```
   $ mkdir v8
   $ cd v8
   $ fetch v8
   ```

4. Sync V8 to the version of V8 currently used by workerd.

   First find, the commit hash of the "v8" `git_repository` in the workerd [WORKSPACE](../WORKSPACE) file.

   Then sync your fetched version v8 so that it corresponds to that hash.

   ```
   $ cd v8/v8
   $ git checkout <commit_hash>
   $ gclient sync
   ```

5. Create a V8 branch for workerd's V8 patches in your recently fetch V8.

   ```
   $ git checkout -b workerd-patches
   $ git am <path_to_workerd>/patches/v8/*
   ```

7. Rebase workerd V8 changes onto the new version of V8. For example, assuming
   we are updating to 11.4.183.8 and there are 8 workerd patches for V8, the
   command would be:

   ```
   $ git rebase --onto 11.4.183.8 HEAD~8
   ```

   There is usually some minor patch editing required during rebase.

   Ideally at this stage, you should be able to build and test V8 with the patches applied.

8. Re-generate workerd's v8 patches. Assuming there are 8 workerd patches for V8,
   the command would be:

   ```
   $ git format-patch -k --no-signature HEAD~8
   ```

9. Remove existing patches from `workerd/patches/v8` and copy the latest patch there.

10. Update the `git_repository` for "v8" in `workerd/WORKSPACE`

    The list of patches should be refreshed if any patches are any new patches or any
    that have been removed.

    The `commit` in the `git_repository` for "v8" should be updated to match
    the version corresponding to the version of v8 being updated. This can be found
    by running `git rev-parse <version>` in the v8 git repo, e.g. `git rev-parse 11.4.183.8`.

    See [V8 git_repository in WORKSPACE](https://github.com/cloudflare/workerd/blob/2d124ecd2d1132537d37bd5e166ac1aec4f7397f/WORKSPACE#L263)

11. Update V8's dependencies in `workerd/WORKSPACE`

   The `v8/.gclient_entries` file contains the commit versions for V8's dependencies.

   You can find V8's dependencies that are carried through to workerd in the `WORKSPACE` file.

   You will usually update `com_google_chromium_icu`, but other projects may need updating
   too. Typically you'll get a build failure if the projects are out of sync. Copy the
   commit versions from `v8/.gclient_entries` to the `WORKSPACE` file.

12. Check workerd's tests pass with the updated V8.

    ```
    $ bazel test //...
    ```

13. Commit your workerd changes and push them for review.
