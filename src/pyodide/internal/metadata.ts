import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';
import { default as PYODIDE_BUCKET } from 'pyodide-internal:generated/pyodide-bucket.json';
// The pyodide-lock.json is read from the Python bundle (pyodide-capnp-bin).
import { default as PYODIDE_LOCK } from 'pyodide-internal:generated/pyodide-lock.json';
import { default as ArtifactBundler } from 'pyodide-internal:artifacts';

export const IS_WORKERD = MetadataReader.isWorkerd();
export const IS_TRACING = MetadataReader.isTracing();
export const SHOULD_SNAPSHOT_TO_DISK = MetadataReader.shouldSnapshotToDisk();
export const IS_CREATING_BASELINE_SNAPSHOT =
  MetadataReader.isCreatingBaselineSnapshot();
export const WORKERD_INDEX_URL = PYODIDE_BUCKET.PYODIDE_PACKAGE_BUCKET_URL;
export const LOAD_WHEELS_FROM_R2: boolean = IS_WORKERD;
export const LOAD_WHEELS_FROM_ARTIFACT_BUNDLER =
  MetadataReader.shouldUsePackagesInArtifactBundler();
export const PACKAGES_VERSION = MetadataReader.getPackagesVersion();
export const LOCKFILE: PackageLock = PYODIDE_LOCK;
export const REQUIREMENTS = MetadataReader.getRequirements();
export const MAIN_MODULE_NAME = MetadataReader.getMainModule();
export const MEMORY_SNAPSHOT_READER = MetadataReader.hasMemorySnapshot()
  ? MetadataReader
  : ArtifactBundler.hasMemorySnapshot()
    ? ArtifactBundler
    : undefined;
