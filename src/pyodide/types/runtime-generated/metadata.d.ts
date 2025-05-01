declare namespace MetadataReader {
  const isWorkerd: () => boolean;
  const isTracing: () => boolean;
  const shouldSnapshotToDisk: () => boolean;
  const isCreatingBaselineSnapshot: () => boolean;
  const getRequirements: () => string[];
  const getMainModule: () => string;
  const getNames: () => string[];
  const getPackageSnapshotImports: (version: string) => string[];
  const getSizes: () => number[];
  const shouldUsePackagesInArtifactBundler: () => boolean;
  const getPyodideVersion: () => string;
  const getPackagesVersion: () => string;
  const getPackagesLock: () => string;
  const read: (index: number, position: number, buffer: Uint8Array) => number;
  const getTransitiveRequirements: () => Set<string>;
  const getDurableObjectClasses: () => string[] | null;
  const getEntrypointClasses: () => string[] | null;
}

export default MetadataReader;
