// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

declare namespace MetadataReader {
  export interface CompatibilityFlags {
    experimental?: boolean;
    python_workflows?: boolean;
    python_no_global_handlers?: boolean;
    python_workers_force_new_vendor_path?: boolean;
    python_dedicated_snapshot?: boolean;
    enable_python_external_sdk?: boolean;
    python_check_rng_state?: boolean;
    python_workflows_implicit_dependencies?: boolean;
    python_process_pth_files?: boolean;
  }

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
  const getCompatibilityFlags: () => CompatibilityFlags;
  const setCpuLimitNearlyExceededCallback: (
    buf: Uint8Array,
    sig_clock: number,
    sig_flag: number
  ) => void;
  const constructor: {
    getBaselineSnapshotImports(): string[];
  };
}

export default MetadataReader;
