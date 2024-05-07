import { createReadonlyFS } from "pyodide-internal:readOnlyFS";

const FSOps = {
  getNodeMode(parent, name, info) {
    return {
      permissions: info.mode,
      isDir: info.children !== undefined,
    };
  },
  setNodeAttributes(node, info, isDir) {
    node.info = info;
    node.modtime = node.info.modtime;
    node.usedBytes = 0;
    if (!isDir) {
      node.contentsOffset = node.info.contentsOffset;
      node.usedBytes = node.info.size;
    }
  },
  readdir(node) {
    return Array.from(node.info.children.keys());
  },
  lookup(parent, name) {
    return parent.info.children.get(name);
  },
  read(stream, position, buffer) {
    return stream.node.info.reader.read(stream.node.contentsOffset + position, buffer);
  },
};

export function createTarFS(Module) {
  return createReadonlyFS(FSOps, Module);
}
