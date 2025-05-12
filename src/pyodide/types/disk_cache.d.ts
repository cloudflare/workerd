declare namespace DiskCache {
  const get: (key: string) => ArrayBuffer | null;
  const put: (key: string, val: ArrayBuffer) => void;
}

export default DiskCache;
