import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';
import { default as ArtifactBundler } from 'pyodide-internal:artifacts';

export const IS_WORKERD = MetadataReader.isWorkerd();
export const IS_TRACING = MetadataReader.isTracing();
export const SHOULD_SNAPSHOT_TO_DISK = MetadataReader.shouldSnapshotToDisk();
export const IS_CREATING_BASELINE_SNAPSHOT =
  MetadataReader.isCreatingBaselineSnapshot();
export const LOAD_WHEELS_FROM_R2: boolean = IS_WORKERD;
export const LOAD_WHEELS_FROM_ARTIFACT_BUNDLER =
  MetadataReader.shouldUsePackagesInArtifactBundler();
export const PACKAGES_VERSION = MetadataReader.getPackagesVersion();
// TODO: pyodide-packages.runtime-playground.workers.dev points at a worker which redirects requests
// to the public R2 bucket URL at pub-45d734c4145d4285b343833ee450ef38.r2.dev. We should remove
// this worker and point at our prod bucket.
export const WORKERD_INDEX_URL =
  PACKAGES_VERSION == '20240829.4'
    ? 'https://pyodide-packages.runtime-playground.workers.dev/' +
      PACKAGES_VERSION +
      '/'
    : 'https://python-packages.edgeworker.net/' + PACKAGES_VERSION + '/';
// The package lock is embedded in the binary. See `getPyodideLock` and `packageLocks`.
export const LOCKFILE: PackageLock = JSON.parse(
  MetadataReader.getPackagesLock()
);
export const REQUIREMENTS = MetadataReader.getRequirements();
export const MAIN_MODULE_NAME = MetadataReader.getMainModule();
export const MEMORY_SNAPSHOT_READER = MetadataReader.hasMemorySnapshot()
  ? MetadataReader
  : ArtifactBundler.hasMemorySnapshot()
    ? ArtifactBundler
    : undefined;
