declare namespace DiskCache {
  const get: (key: String) => ArrayBuffer | null;
  const put: (key: String, val: ArrayBuffer) => void;
}

export default DiskCache;
