import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';
import { default as ArtifactBundler } from 'pyodide-internal:artifacts';

export const IS_WORKERD = MetadataReader.isWorkerd();
export const IS_TRACING = MetadataReader.isTracing();

// Snapshots
export const SHOULD_SNAPSHOT_TO_DISK = MetadataReader.shouldSnapshotToDisk();
export const IS_CREATING_BASELINE_SNAPSHOT =
  MetadataReader.isCreatingBaselineSnapshot();
export const IS_EW_VALIDATING = ArtifactBundler.isEwValidating();
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
// TODO: pyodide-packages.runtime-playground.workers.dev points at a worker which redirects requests
// to the public R2 bucket URL at pub-45d734c4145d4285b343833ee450ef38.r2.dev. We should remove
// this worker and point at our prod bucket.
export const WORKERD_INDEX_URL = USING_OLDEST_PACKAGES_VERSION
  ? 'https://pyodide-packages.runtime-playground.workers.dev/' +
    PACKAGES_VERSION +
    '/'
  : 'https://python-packages.edgeworker.net/' + PACKAGES_VERSION + '/';
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
const EXTERNAL_SDK = !!COMPATIBILITY_FLAGS.enable_python_external_sdk;
export const WORKFLOWS_IMPLICIT_DEPS =
  !!COMPATIBILITY_FLAGS.python_workflows_implicit_dependencies;

export const LEGACY_GLOBAL_HANDLERS = !NO_GLOBAL_HANDLERS;
export const LEGACY_VENDOR_PATH = !FORCE_NEW_VENDOR_PATH;
export const LEGACY_INCLUDE_SDK = !EXTERNAL_SDK;
export const CHECK_RNG_STATE = !!COMPATIBILITY_FLAGS.python_check_rng_state;

export const setCpuLimitNearlyExceededCallback =
  MetadataReader.setCpuLimitNearlyExceededCallback.bind(MetadataReader);
