declare namespace MetadataReader {
  const isWorkerd: () => boolean;
  const isTracing: () => boolean;
  const shouldSnapshotToDisk: () => boolean;
  const isCreatingBaselineSnapshot: () => boolean;
  const getRequirements: () => Array<string>;
  const getMainModule: () => string;
  const hasMemorySnapshot: () => boolean;
}

export default MetadataReader;
