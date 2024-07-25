declare namespace ArtifactBundler {
  type MemorySnapshotResult = {
    snapshot: Uint8Array;
    importedModulesList: Array<string>;
  }

  const hasMemorySnapshot: () => boolean;
  const isEwValidating: () => boolean;
  const isEnabled: () => boolean;
  const uploadMemorySnapshot: (toUpload: Uint8Array) => Promise<boolean>;
  const readMemorySnapshot: (
    offset: number,
    buf: Uint32Array | Uint8Array,
  ) => void;
  const getMemorySnapshotSize: () => number;
  const disposeMemorySnapshot: () => void;
  const storeMemorySnapshot: (snap: MemorySnapshotResult) => void;
}

export default ArtifactBundler;
