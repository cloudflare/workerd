
declare namespace DiskCache {
  const get: (key: String) => ArrayBuffer;
  const put: (key: String, val: ArrayBuffer) => void;
}

export default DiskCache;
