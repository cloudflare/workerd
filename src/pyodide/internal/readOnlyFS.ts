type ReadFn<Info> = FSStreamOps<Info>['read'];

// When we load shared libraries we need to ensure they come from a read only file system.

// Map to store the original trusted read function for each read-only filesystem. We store the
// function itself to prevent attacks where user code modifies stream_ops.read after filesystem
// creation and tricks us into loading a dynamically generated so file.
const TRUSTED_READ_FUNCS: Map<object, ReadFn<any>> = new Map();

export function getTrustedReadFunc<Info>(
  node: FSNode<Info>
): ReadFn<Info> | undefined {
  return TRUSTED_READ_FUNCS.get(node.mount.type);
}

export function createReadonlyFS<Info>(
  FSOps: FSOps<Info>,
  Module: Module
): EmscriptenFS<Info> {
  const FS = Module.FS;
  const ReadOnlyFS: EmscriptenFS<Info> = {
    mount(mount) {
      return ReadOnlyFS.createNode(null, '/', mount.opts.info);
    },
    createNode(parent, name, info): FSNode<Info> {
      // eslint-disable-next-line prefer-const
      let { permissions: mode, isDir } = FSOps.getNodeMode(parent, name, info);
      if (isDir) {
        mode |= 1 << 14; // set S_IFDIR
      } else {
        mode |= 1 << 15; // set S_IFREG
      }
      const node = FS.createNode(parent, name, mode);
      node.node_ops = ReadOnlyFS.node_ops;
      node.stream_ops = ReadOnlyFS.stream_ops;
      FSOps.setNodeAttributes(node, info, isDir);
      return node;
    },
    node_ops: {
      getattr(node) {
        const size = node.usedBytes;
        const mode = node.mode;
        const t = new Date(node.modtime);
        const blksize = 4096;
        const blocks = ((size + blksize - 1) / blksize) | 0;
        return {
          dev: 1,
          ino: node.id,
          mode,
          nlink: 1,
          uid: 0,
          gid: 0,
          rdev: 0,
          size,
          atime: t,
          mtime: t,
          ctime: t,
          blksize,
          blocks,
        };
      },
      readdir(node) {
        return FSOps.readdir(node);
      },
      lookup(parent, name) {
        const child = FSOps.lookup(parent, name);
        if (child === undefined) {
          throw FS.genericErrors?.[44] ?? new FS.ErrnoError(44); // ENOENT
        }
        return ReadOnlyFS.createNode(parent, name, child);
      },
    },
    stream_ops: {
      llseek(stream, offset, whence) {
        let position = offset;
        if (whence === 1) {
          // SEEK_CUR
          position += stream.position;
        } else if (whence === 2) {
          // SEEK_END
          if (FS.isFile(stream.node.mode)) {
            position += stream.node.usedBytes;
          }
        }
        return position;
      },
      read(stream, buffer, offset, length, position) {
        if (position >= stream.node.usedBytes) return 0;
        const size = Math.min(stream.node.usedBytes - position, length);
        buffer = buffer.subarray(offset, offset + size);
        return FSOps.read(stream, position, buffer);
      },
    },
  };
  // Register this filesystem as read-only and store its trusted read function so we can load so
  // files from it.
  TRUSTED_READ_FUNCS.set(ReadOnlyFS, ReadOnlyFS.stream_ops.read);
  return ReadOnlyFS;
}
