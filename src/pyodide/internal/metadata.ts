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

export interface CompatibilityFlags {
  python_workflows?: boolean;
  python_no_global_handlers?: boolean;
  python_workers_force_new_vendor_path?: boolean;
}

export interface CloudflareGlobal {
  Cloudflare?: {
    compatibilityFlags?: CompatibilityFlags;
  };
}

const compatibilityFlags: CompatibilityFlags =
  (globalThis as CloudflareGlobal)?.Cloudflare?.compatibilityFlags ?? {};
export const workflowsEnabled: boolean = !!compatibilityFlags.python_workflows;
export const legacyGlobalHandlers: boolean =
  !compatibilityFlags.python_no_global_handlers;
export const legacyVendorPath: boolean =
  !compatibilityFlags.python_workers_force_new_vendor_path;
