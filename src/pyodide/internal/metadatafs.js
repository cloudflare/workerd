import { default as MetadataReader } from "pyodide-internal:runtime-generated/metadata";
import { createReadonlyFS } from "pyodide-internal:readOnlyFS";

export function createMetadataFS(Module) {
  const TIMESTAMP = Date.now();
  const names = MetadataReader.getNames();
  const sizes = MetadataReader.getSizes();
  const nameToIndex = new Map(names.map((val, idx) => [val, idx]));

  const FSOps = {
    getNodeMode(parent, name, index) {
      return {
        permissions: 0o555, // read and execute but not write
        isDir: index === undefined,
      };
    },
    setNodeAttributes(node, index, isDir) {
      node.modtime = TIMESTAMP;
      node.usedBytes = 0;
      if (!isDir) {
        node.index = index;
        node.usedBytes = sizes[index];
      }
    },
    readdir(node) {
      return names;
    },
    lookup(parent, name) {
      return nameToIndex.get(name);
    },
    read(stream, position, buffer) {
      return MetadataReader.read(stream.node.index, position, buffer);
    },
  };

  return createReadonlyFS(FSOps, Module);
}
