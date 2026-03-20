// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare namespace ArtifactBundler {
  type SnapshotType = 'baseline' | 'dedicated' | 'package';
  type MemorySnapshotResult = {
    snapshot: Uint8Array;
    importedModulesList: string[];
    snapshotType: SnapshotType;
  };

  const hasMemorySnapshot: () => boolean;
  const isEwValidating: () => boolean;
  const readMemorySnapshot: (
    offset: number,
    buf: Uint32Array | Uint8Array
  ) => void;
  const getMemorySnapshotSize: () => number;
  const disposeMemorySnapshot: () => void;
  const storeMemorySnapshot: (snap: MemorySnapshotResult) => void;
  const getPackage: (path: string) => Reader | null;
}

export default ArtifactBundler;
