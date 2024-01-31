export function createReadonlyFS(FSOps, Module) {
  const FS = Module.FS;
  const ReadOnlyFS = {
    mount(mount) {
      return ReadOnlyFS.createNode(null, "/", mount.opts.info);
    },
    createNode(parent, name, info) {
      let { permissions: mode, isDir } = FSOps.getNodeMode(parent, name, info);
      if (isDir) {
        mode |= 1 << 14; // set S_IFDIR
      } else {
        mode |= 1 << 15; // set S_IFREG
      }
      var node = FS.createNode(parent, name, mode);
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
          throw FS.genericErrors[44]; // ENOENT
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
            position += stream.node.info.size;
          }
        }
        return position;
      },
      read(stream, buffer, offset, length, position) {
        if (position >= stream.node.usedBytes) return 0;
        var size = Math.min(stream.node.usedBytes - position, length);
        buffer = buffer.subarray(offset, offset + size);
        return FSOps.read(stream, position, buffer);
      },
    },
  };
  return ReadOnlyFS;
}
