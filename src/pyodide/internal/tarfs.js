import { default as TarReader } from "pyodide-internal:packages_tar_reader";

export function createTarFS(Module) {
  const FS = Module.FS;
  const TARFS = {
    mount(mount) {
      return TARFS.createNode(null, "/", mount.opts.info);
    },
    createNode(parent, name, info) {
      let mode = info.mode;
      const isDir = info.children !== undefined;
      if (isDir) {
        mode |= 1 << 14; // set S_IFDIR
      } else {
        mode |= 1 << 15; // set S_IFREG
      }
      var node = FS.createNode(parent, name, mode);
      node.node_ops = TARFS.node_ops;
      node.stream_ops = TARFS.stream_ops;
      node.info = info;
      if (!isDir) {
        node.contentsOffset = node.info.contentsOffset;
        node.usedBytes = node.info.size;
      }
      return node;
    },
    node_ops: {
      getattr(node) {
        const size = node.info.size;
        const mode = node.mode;
        const t = new Date(node.info.modtime);
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
        return Array.from(node.info.children.keys());
      },
      lookup(parent, name) {
        const child = parent.info.children.get(name);
        if (!child) {
          throw FS.genericErrors[44]; // ENOENT
        }
        return TARFS.createNode(parent, name, child);
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
        var contentsOffset = stream.node.contentsOffset;
        if (position >= stream.node.usedBytes) return 0;
        var size = Math.min(stream.node.usedBytes - position, length);
        buffer = buffer.subarray(offset, offset + size);
        TarReader.read(contentsOffset, buffer);
        return size;
      },
    },
  };
  return TARFS;
}
