import { default as MetadataReader } from 'pyodide-internal:runtime-generated/metadata';
import { createReadonlyFS } from 'pyodide-internal:readOnlyFS';

function createTree(paths: string[]): MetadataFSInfo {
  const tree = new Map();
  paths.forEach((elt: string, idx: number) => {
    let subTree = tree;
    const parts = elt.split('/');
    const name = parts.pop();
    for (const part of parts) {
      let next = subTree.get(part);
      if (!next) {
        next = new Map();
        subTree.set(part, next);
      }
      subTree = next;
    }
    subTree.set(name, idx);
  });
  return tree;
}

export function createMetadataFS(Module: Module): object {
  // TODO: Make this type more specific
  const TIMESTAMP = Date.now();
  const names = MetadataReader.getNames();
  const sizes = MetadataReader.getSizes();
  const rootInfo = createTree(names);
  const FSOps: FSOps<MetadataFSInfo> = {
    getNodeMode(parent, name, info) {
      return {
        permissions: 0o555, // read and execute but not write
        isDir: typeof info !== 'number',
      };
    },
    setNodeAttributes(node, info, isDir) {
      node.modtime = TIMESTAMP;
      node.usedBytes = 0;
      if (info === undefined) {
        info = rootInfo;
      }
      if (isDir) {
        node.tree = info as MetadataDirInfo;
      } else {
        node.index = info as number;
        node.usedBytes = sizes[info as number];
      }
    },
    readdir(node) {
      if (node.tree == undefined) {
        throw new Error('cannot read directory, tree is undefined');
      }
      return Array.from(node.tree.keys());
    },
    lookup(parent, name) {
      if (parent.tree == undefined) {
        throw new Error('cannot lookup directory, tree is undefined');
      }
      const res = parent.tree.get(name);
      if (res === undefined) {
        throw new Module.FS.ErrnoError(44);
      }
      return res;
    },
    read(stream, position, buffer) {
      return MetadataReader.read(stream.node.index!, position, buffer);
    },
  };

  return createReadonlyFS(FSOps, Module);
}
