declare namespace DiskCache {
  const get: (key: string) => ArrayBuffer | null;
  const put: (key: string, val: ArrayBuffer | Uint8Array) => void;
  const putSnapshot: (key: string, val: ArrayBuffer | Uint8Array) => void;
}

export default DiskCache;
