// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';
import { default as ArtifactBundler } from 'pyodide-internal:artifacts';

export const IS_WORKERD = MetadataReader.isWorkerd();
export const IS_TRACING = MetadataReader.isTracing();
export const SHOULD_ABORT_ISOLATE_ON_FATAL_ERROR =
  MetadataReader.shouldAbortIsolateOnFatalError();

// Snapshots
export const SHOULD_SNAPSHOT_TO_DISK = MetadataReader.shouldSnapshotToDisk();
export const IS_CREATING_BASELINE_SNAPSHOT =
  MetadataReader.isCreatingBaselineSnapshot();
export const IS_EW_VALIDATING = ArtifactBundler.isEwValidating();
export const IS_DYNAMIC_WORKER = ArtifactBundler.isDynamicWorker();
export const IS_CREATING_SNAPSHOT = IS_EW_VALIDATING || SHOULD_SNAPSHOT_TO_DISK;

// There are two validations that we perform. The first one runs without a _bundled_ memory snapshot
// (but may use a baseline snapshot, though this is stored in the ArtfactBundler). The second one
// runs with a _bundled_ memory snapshot, which is expected to be a dedicated snapshot. When this
// bool is true, we verify that the bundled memory snapshot we receive is a dedicated snapshot.
export const IS_SECOND_VALIDATION_PHASE =
  MetadataReader.hasMemorySnapshot() && IS_EW_VALIDATING;
export const MEMORY_SNAPSHOT_READER = MetadataReader.hasMemorySnapshot()
  ? MetadataReader
  : ArtifactBundler.hasMemorySnapshot()
    ? ArtifactBundler
    : undefined;

// Packages
export const PACKAGES_VERSION = MetadataReader.getPackagesVersion();
export const USING_OLDEST_PACKAGES_VERSION = PACKAGES_VERSION === '20240829.4';
// The package lock is embedded in the binary. See `getPyodideLock` and `packageLocks`.
export const LOCKFILE = JSON.parse(
  MetadataReader.getPackagesLock()
) as PackageLock;

export const REQUIREMENTS = MetadataReader.getRequirements();
export const TRANSITIVE_REQUIREMENTS =
  MetadataReader.getTransitiveRequirements();

// Entrypoints
export const MAIN_MODULE_NAME = MetadataReader.getMainModule();

export type CompatibilityFlags = MetadataReader.CompatibilityFlags;
export const COMPATIBILITY_FLAGS: MetadataReader.CompatibilityFlags = {
  // Compat flags returned from getCompatibilityFlags is immutable,
  // but in Pyodide 0.26, we modify the JS object that is exposed to the Python through
  // registerJsModule so we create a new object here by copying the values.
  ...MetadataReader.getCompatibilityFlags(),
};
export const WORKFLOWS_ENABLED: boolean =
  !!COMPATIBILITY_FLAGS.python_workflows;
const NO_GLOBAL_HANDLERS: boolean =
  !!COMPATIBILITY_FLAGS.python_no_global_handlers;
const FORCE_NEW_VENDOR_PATH: boolean =
  !!COMPATIBILITY_FLAGS.python_workers_force_new_vendor_path;
export const IS_DEDICATED_SNAPSHOT_ENABLED: boolean =
  !!COMPATIBILITY_FLAGS.python_dedicated_snapshot;
export const EXTERNAL_SDK = !!COMPATIBILITY_FLAGS.enable_python_external_sdk;

export const LEGACY_GLOBAL_HANDLERS = !NO_GLOBAL_HANDLERS;
export const LEGACY_VENDOR_PATH = !FORCE_NEW_VENDOR_PATH;
export const CHECK_RNG_STATE = !!COMPATIBILITY_FLAGS.python_check_rng_state;
export const PROCESS_PTH_FILES = !!COMPATIBILITY_FLAGS.python_process_pth_files;

export const setCpuLimitNearlyExceededCallback =
  MetadataReader.setCpuLimitNearlyExceededCallback.bind(MetadataReader);
