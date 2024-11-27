// This is based on the info about the tar file format on wikipedia
// And some trial and error with real tar files.
// https://en.wikipedia.org/wiki/Tar_(computing)#File_format

const decoder = new TextDecoder();
function decodeString(buf: Uint8Array): string {
  const nullIdx = buf.indexOf(0);
  if (nullIdx >= 0) {
    buf = buf.subarray(0, nullIdx);
  }
  return decoder.decode(buf);
}
function decodeField(buf: Uint8Array, offset: number, size: number): string {
  return decodeString(buf.subarray(offset, offset + size));
}
function decodeNumber(buf: Uint8Array, offset: number, size: number): number {
  return parseInt(decodeField(buf, offset, size), 8);
}

function decodeHeader(buf: Uint8Array, reader: Reader): TarFSInfo {
  const nameBase = decodeField(buf, 0, 100);
  const namePrefix = decodeField(buf, 345, 155);
  let path = namePrefix + nameBase;
  // Trim possible leading ./
  if (path.startsWith('./')) {
    path = path.slice(2);
  }
  const mode = decodeNumber(buf, 100, 8);
  const size = decodeNumber(buf, 124, 12);
  const modtime = decodeNumber(buf, 136, 12);
  const type = String.fromCharCode(buf[156]);
  return {
    path,
    name: path,
    mode,
    size,
    modtime,
    type,
    parts: [],
    children: undefined,
    reader,
  };
}

export function parseTarInfo(reader: Reader): [TarFSInfo, string[]] {
  const directories: TarFSInfo[] = [];
  const soFiles = [];
  const root: TarFSInfo = {
    children: new Map(),
    mode: 0o777,
    type: '5',
    modtime: 0,
    size: 0,
    path: '',
    name: '',
    parts: [],
    reader,
  };
  let directory = root;
  const buf = new Uint8Array(512);
  let offset = 0;
  let longName = null; // if truthy, overwrites the filename of the next header
  while (true) {
    reader.read(offset, buf);
    const info = decodeHeader(buf, reader);
    if (isNaN(info.mode)) {
      // Invalid mode means we're done
      return [root, soFiles];
    }
    if (longName) {
      info.path = longName;
      info.name = longName;
      longName = null;
    }
    const contentsOffset = offset + 512;
    offset += 512 * Math.ceil(info.size / 512 + 1);
    if (info.path === '') {
      // skip possible leading ./ directory
      continue;
    }
    if (info.path.includes('PaxHeader')) {
      // Ignore PaxHeader extension
      // These metadata directories don't actually have a directory entry which
      // is going to cause us to crash below.
      // Our tar files shouldn't have these anyways...
      continue;
    }
    if (info.type === 'L') {
      const buf = new Uint8Array(info.size);
      reader.read(contentsOffset, buf);
      longName = decodeString(buf);
      continue;
    }

    // Navigate to the correct directory by going up until we're at the common
    // ancestor of the current position and the target then back down.
    //
    // Most tar files I run into are lexicographically sorted, so the "go back
    // down" step is not necessary. But some tar files are a bit out of order.
    //
    // We do rely on the fact that the entry for a given directory appears
    // before any files in the directory. I don't see anywhere in the spec where
    // it says this is required but I think it would be weird and annoying for a
    // tar file to violate this property.

    // go up to common ancestor
    while (directories.length && !info.name.startsWith(directory.path)) {
      directory = directories.pop()!;
    }
    // go down to target (in many tar files this second loop body is evaluated 0
    // times)
    const parts = info.path.slice(0, -1).split('/');
    for (let i = directories.length; i < parts.length - 1; i++) {
      directories.push(directory);
      directory = directory.children!.get(parts[i])!;
    }
    if (info.type === '5') {
      // a directory
      directories.push(directory);
      info.parts = parts;
      info.name = info.parts.at(-1)!;
      info.children = new Map();
      directory.children!.set(info.name, info);
      directory = info;
    } else if (info.type === '0') {
      // a normal file
      info.contentsOffset = contentsOffset;
      info.name = info.path.slice(directory.path.length);
      if (info.name.endsWith('.so')) {
        soFiles.push(info.path);
      }
      directory.children!.set(info.name, info);
    } else {
      // fail if we encounter other values of type (e.g., symlink, LongName, etc)
      throw new Error(`Python TarFS error: Unexpected type ${info.type}`);
    }
  }
}
