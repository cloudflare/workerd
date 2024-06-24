declare namespace ArtifactBundler {
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
  const storeMemorySnapshot: (snap: Uint8Array) => void;
}

export default ArtifactBundler;
