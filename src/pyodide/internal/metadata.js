import { default as MetadataReader } from "pyodide-internal:runtime-generated/metadata";
export { default as LOCKFILE } from "pyodide-internal:generated/pyodide-lock.json";
import { default as PYODIDE_BUCKET } from "pyodide-internal:generated/pyodide-bucket.json";
import { default as ArtifactBundler } from "pyodide-internal:artifacts";

export const IS_WORKERD = MetadataReader.isWorkerd();
export const IS_TRACING = MetadataReader.isTracing();
export const WORKERD_INDEX_URL = PYODIDE_BUCKET.PYODIDE_PACKAGE_BUCKET_URL;
export const REQUIREMENTS = MetadataReader.getRequirements();
export const MAIN_MODULE_NAME = MetadataReader.getMainModule();
export const MEMORY_SNAPSHOT_READER = MetadataReader.hasMemorySnapshot()
  ? MetadataReader
  : ArtifactBundler.hasMemorySnapshot()
    ? ArtifactBundler
    : undefined;
