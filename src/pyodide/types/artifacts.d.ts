type ValidatorSnapshotUploader = {
  storeMemorySnapshot: (snap: Uint8Array) => void;
};

type RuntimeSnapshotUploader = {
  uploadMemorySnapshot: (snap: Uint8Array) => boolean;
};

type SnapshotDownloader = {
  getMemorySnapshotSize: () => number;
  disposeMemorySnapshot: () => void;
  readMemorySnapshot: (
    offset: number,
    buf: Uint32Array | Uint8Array,
  ) => void;
};

declare namespace Artifacts {
  const validatorSnapshotUploader: ValidatorSnapshotUploader | undefined;
  const runtimeSnapshotUploader: RuntimeSnapshotUploader | undefined;
  const snapshotDownloader: SnapshotDownloader | undefined;
}

export default Artifacts;

