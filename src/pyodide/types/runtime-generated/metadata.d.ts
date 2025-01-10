declare namespace MetadataReader {
  const isWorkerd: () => boolean;
  const isTracing: () => boolean;
  const shouldSnapshotToDisk: () => boolean;
  const isCreatingBaselineSnapshot: () => boolean;
  const getRequirements: () => string[];
  const getMainModule: () => string;
  const hasMemorySnapshot: () => boolean;
  const getNames: () => string[];
  const getWorkerFiles: (ext: string) => string[];
  const getSizes: () => number[];
  const readMemorySnapshot: (
    offset: number,
    buf: Uint32Array | Uint8Array
  ) => void;
  const getMemorySnapshotSize: () => number;
  const disposeMemorySnapshot: () => void;
  const shouldUsePackagesInArtifactBundler: () => boolean;
  const getPackagesVersion: () => string;
  const read: (index: number, position: number, buffer: Uint8Array) => number;
}

export default MetadataReader;
