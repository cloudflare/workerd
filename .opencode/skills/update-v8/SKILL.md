---
name: update-v8
description: Step-by-step guide for updating the V8 JavaScript engine in workerd, including patch rebasing, dependency updates, integrity hashes, and verification. Load this skill when performing or assisting with a V8 version bump.
---

## Updating V8 in workerd

V8 updates are high-risk changes that require careful patch management and human judgment for merge conflicts. This skill covers the full process. **Always confirm the target version with the developer before starting.**

See also: `docs/v8-updates.md` for the original reference document.

**Always** communicate and confirm with the developer at each step.
**Never** take irreversible actions (like dropping patches or updating hashes) without explicit confirmation.

---

### Prerequisites

- `depot_tools` installed and on `$PATH` ([setup guide](https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html#_setting_up))
- A local V8 checkout (outside the workerd repo to avoid confusing Bazel):
  ```sh
  mkdir v8 && cd v8 && fetch v8
  ```

---

### Step 1: Identify the target version

Check [chromiumdash.appspot.com](https://chromiumdash.appspot.com/) for the latest V8 version used by Chrome Beta. Confirm the target `<new_version>` with the developer.

Find the current version in `build/deps/v8.MODULE.bazel`:

```
VERSION = "14.5.201.6"    # example — check the actual file
```

We'll call this `<old_version>`.

### Step 2: Sync local V8 to the current workerd version

```sh
cd <path_to_v8>/v8
git checkout <old_version>
gclient sync
```

### Step 3: Apply workerd's patches onto a branch

```sh
git checkout -b workerd-patches
git am <path_to_workerd>/patches/v8/*
```

There are multiple patches in `patches/v8/`. These include workerd-specific customizations:

| Patch category  | Examples                                                                                              |
| --------------- | ----------------------------------------------------------------------------------------------------- |
| Serialization   | Custom ValueSerializer/Deserializer format versions, proxy/function host object support               |
| Build system    | Windows/Bazel fixes, shared linkage, dependency path overrides (fp16, fast_float, simdutf, dragonbox) |
| Embedder hooks  | Promise context tagging, cross-request promise resolution, extra isolate embedder slot                |
| Bug workarounds | Memory leak assert disable, slow handle check disable, builtin-can-allocate workaround                |
| API additions   | `String::IsFlat`, `AdjustAmountOfExternalAllocatedMemory` exposure, additional Exception constructors |
| ICU/config      | ICU data export, googlesource ICU binding, verify_write_barriers flag                                 |

### Step 4: Rebase patches onto the new V8 version

```sh
git rebase --onto <new_version> <old_version>
```

**This is where most of the work happens.** Expect conflicts. Key guidance:

- **Build-system patches** (dependency paths, Bazel config) conflict most often as upstream V8 restructures its build.
- **API patches** (new methods on V8 classes) may conflict if upstream changed the surrounding code.
- **Always preserve workerd's intent** — understand what each patch does before resolving conflicts. The patch filenames are descriptive.
- **Do not drop patches** without explicit confirmation from the developer.
- **Do not auto-resolve conflicts** — flag them for human review. Merge conflicts in V8 patches almost always require human judgment.

### Step 5: Regenerate patches

```sh
git format-patch --full-index -k --no-signature --no-stat --zero-commit <new_version>
```

This produces numbered `.patch` files in the current directory.

### Step 6: Replace patches in workerd

**Always** confirm with the human before replacing patches. If any patches were dropped or added,
the human needs to review the changes.

```sh
rm <path_to_workerd>/patches/v8/*
cp *.patch <path_to_workerd>/patches/v8/
```

### Step 7: Update `build/deps/v8.MODULE.bazel`

Three things need updating:

1. **`VERSION`**: Set to `<new_version>`.

2. **`INTEGRITY`**: Compute the sha256 hash of the new tarball:

   ```sh
   curl -sL "https://github.com/v8/v8/archive/refs/tags/<new_version>.tar.gz" -o v8.tar.gz
   openssl dgst -sha256 -binary v8.tar.gz | openssl base64 -A
   ```

   Format: `"sha256-<base64_hash>="`. Alternatively, attempt a build and copy the expected hash from Bazel's mismatch error.

3. **`PATCHES`**: Update the list if patches were added, removed, or renamed. The list must match the filenames in `patches/v8/` exactly, in order.

### Step 8: Update V8's dependencies

V8 depends on several libraries that are pinned in `build/deps/v8.MODULE.bazel` and `build/deps/deps.jsonc`. Check the local V8 checkout's `DEPS` file for commit versions:

```sh
cat <path_to_v8>/v8/DEPS
```

Dependencies to check and update:

| Dependency                      | Where                                      | Notes                                                          |
| ------------------------------- | ------------------------------------------ | -------------------------------------------------------------- |
| `com_googlesource_chromium_icu` | `v8.MODULE.bazel` (git_repository commit)  | Chromium fork; update commit from V8's DEPS                    |
| `perfetto`                      | `deps.jsonc` (managed by `update-deps.py`) | V8 depends via Chromium; safe to bump to latest GitHub release |
| `simdutf`                       | `deps.jsonc` (managed by `update-deps.py`) | V8 depends via Chromium; safe to bump to latest GitHub release |

For dependencies in `deps.jsonc`, you can use the update script:

```sh
python3 build/deps/update-deps.py perfetto
python3 build/deps/update-deps.py simdutf
```

This fetches the latest version, computes integrity hashes, and regenerates the `gen/` MODULE.bazel fragments. **Do not hand-edit files in `build/deps/gen/`.**

### Step 9: Build and test

```sh
# Full build
just build

# Full test suite
just test
```

Watch for:

- **Build failures from V8 API changes**: V8 may deprecate or change APIs between versions. Search for deprecation warnings in the build output. Common areas affected:
  - `src/workerd/jsg/` — V8 binding layer, most directly affected
  - `src/workerd/api/` — APIs that interact with V8 types directly
  - `src/workerd/io/worker.c++` — Isolate creation and configuration

- **Test failures from behavior changes**: V8 may change observable JS behavior. Check:
  - `just test //src/workerd/jsg/...` — JSG binding tests
  - `just test //src/workerd/api/tests/...` — API tests
  - Node.js compatibility tests (`just node-test`)
  - Web Platform Tests (`just wpt-test`)

- **New V8 deprecation warnings**: These are future breakage signals. Document them in the PR description even if tests pass.

### Step 10: Commit and submit

Prompt the user to commit the changes and push for review.

**Never** push the branch for review without a human review of the patch changes.

You **May** prepare the draft PR text for the user. The PR should include:

- Updated `build/deps/v8.MODULE.bazel` (version, integrity, patches list)
- Updated patches in `patches/v8/`
- Updated dependency versions if changed
- Any C++ fixes for V8 API changes
- PR description listing: old version, new version, patches that required conflict resolution, any deprecation warnings observed, and any behavior changes noted

---

### Checklist

- [ ] Target V8 version confirmed with developer
- [ ] Local V8 checked out and synced to old version
- [ ] workerd patches applied and rebased onto new version
- [ ] Conflicts resolved with human review (no auto-resolution)
- [ ] Patches regenerated with `git format-patch`
- [ ] Old patches replaced with new patches in `patches/v8/`
- [ ] `VERSION` updated in `v8.MODULE.bazel`
- [ ] `INTEGRITY` updated in `v8.MODULE.bazel`
- [ ] `PATCHES` list updated if patches added/removed/renamed
- [ ] V8 dependencies checked and updated (ICU, perfetto, simdutf)
- [ ] `just build` succeeds
- [ ] `just test` passes (or failures documented and explained)
- [ ] No new patches dropped without explicit confirmation
- [ ] PR description documents version change, conflict resolutions, and deprecation warnings

---

### Troubleshooting

**Bazel integrity mismatch**: If you see `expected sha256-... but got sha256-...`, copy the "got" hash into the `INTEGRITY` field. This happens when the hash was computed incorrectly or the tarball was re-generated by GitHub.

**Patch won't apply**: A patch that applied cleanly during `git am` but fails in Bazel means the `git format-patch` output differs from what Bazel expects. Verify you used `--full-index -k --no-signature --no-stat --zero-commit` flags. Also verify patch order matches the `PATCHES` list.

**ICU build failures**: ICU is a Chromium fork fetched via `git_repository`. If the commit in `v8.MODULE.bazel` is wrong, you'll see missing-file or compilation errors in ICU. Cross-reference with V8's `DEPS` file for the correct commit.

**`update-deps.py` fails**: The script requires network access to fetch versions. If a dependency's GitHub release format changed, you may need to update the version manually in `deps.jsonc` and run `python3 build/deps/update-deps.py` to regenerate hashes.
