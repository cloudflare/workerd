declare module 'pyodide-internal:runtime-generated/metadata' {
  namespace MetadataReader {
    const isWorkerd: () => boolean;
    const isTracing: () => boolean;
    const shouldSnapshotToDisk: () => boolean;
    const isCreatingBaselineSnapshot: () => boolean;
    const getRequirements: () => string[];
    const getMainModule: () => string;
    const hasMemorySnapshot: () => boolean;
    const getNames: () => string[];
    const getPackageSnapshotImports: (version: string) => string[];
    const getSizes: () => number[];
    const readMemorySnapshot: (
      offset: number,
      buf: Uint32Array | Uint8Array
    ) => void;
    const getMemorySnapshotSize: () => number;
    const disposeMemorySnapshot: () => void;
    const getPyodideVersion: () => string;
    const getPackagesVersion: () => string;
    const getPackagesLock: () => string;
    const read: (index: number, position: number, buffer: Uint8Array) => number;
    const getTransitiveRequirements: () => Set<string>;
    const constructor: {
      getBaselineSnapshotImports(): string[];
    };
  }

  export default MetadataReader;
}
