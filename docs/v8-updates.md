# V8 Updates

To update the version of V8 used by workerd, the steps are:

1. Check <https://omahaproxy.appspot.com/> and identify the latest version of V8 used by the beta versions of Chrome beta.

2. Install depot_tools if it is not already present on your machine.

   <https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html#_setting_up>

3. Fetch a local copy of V8:

   ```sh
   mkdir v8
   cd v8
   fetch v8
   ```

4. Sync the local copy of V8 to the version used by workerd.

   First find, the commit hash of the "v8" `git_repository` in the workerd [WORKSPACE](../WORKSPACE) file.

   Then sync your fetched version v8 so that it corresponds to that hash.

   ```sh
   cd v8/v8
   git checkout <commit_hash>
   gclient sync
   ```

5. Create a V8 branch for workerd's V8 patches in your local copy of V8.

   ```sh
   git checkout -b workerd-patches
   git am <path_to_workerd>/patches/v8/*
   ```

6. Rebase the workerd V8 changes onto the new version of V8. For example, assuming
   we are updating to 11.4.183.8 and there are 8 workerd patches for V8, the
   command would be:

   ```sh
   git rebase --onto 11.4.183.8 HEAD~8
   ```

   There is usually some minor patch editing required during a rebase.

   Ideally at this stage, you should be able to build and test the local V8 with the
   patches applied. See the V8 [Testing](https://v8.dev/docs/test) page.

7. Re-generate workerd's V8 patches. Assuming there are 8 workerd patches for V8,
   the command would be:

   ```sh
   git format-patch --full-index -k --no-signature HEAD~8
   ```

8. Remove the existing patches from `workerd/patches/v8` and copy the latest patches
   for the V8 directory there.

9. Update the `git_repository` for V8 in the `workerd/WORKSPACE` file.

    The list of patches should be refreshed if new patches are being added or existing
    patches are being removed.

    The `commit` in the `git_repository` for V8 should be updated to match
    the version corresponding to the version of V8 being updated. This can be found
    by running `git rev-parse <version>` in the local V8 git repo, e.g. `git rev-parse 11.4.183.8`.

    See [V8 git_repository in WORKSPACE](https://github.com/cloudflare/workerd/blob/2d124ecd2d1132537d37bd5e166ac1aec4f7397f/WORKSPACE#L263)

10. Update V8's dependencies in `workerd/WORKSPACE`.

    The `v8/.gclient_entries` file contains the commit versions for V8's dependencies.

    You can find V8's dependencies that are carried through to workerd in the `WORKSPACE` file.

    You will usually update `com_google_chromium_icu`, but other projects may need updating
    too. Typically you'll get a build failure if the projects are out of sync. Copy the
    commit versions from `v8/.gclient_entries` to the `WORKSPACE` file.

11. Check workerd's tests pass with the updated V8.

     ```sh
     bazel test //...
     ```

    You may see advice in the build output about shallow-since dates for the V8 related
    git repositories:

    ```sh
    DEBUG: Rule 'v8' indicated that a canonical reproducible form can be obtained by modifying arguments shallow_since = "1683898886 +0000"
    ```

    You can follow the advice in these messages and update the shallow-since dates for
    the V8 related git repositories in the WORKSPACE file.

12. Commit your workerd changes and push them for review.
