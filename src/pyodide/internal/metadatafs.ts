// @ts-nocheck
import { default as MetadataReader } from "pyodide-internal:runtime-generated/metadata";
import { createReadonlyFS } from "pyodide-internal:readOnlyFS";

function createTree(paths) {
  const tree = new Map();
  paths.forEach((elt, idx) => {
    let subTree = tree;
    const parts = elt.split("/");
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

export function createMetadataFS(Module) {
  const TIMESTAMP = Date.now();
  const names = MetadataReader.getNames();
  const sizes = MetadataReader.getSizes();
  const rootInfo = createTree(names);
  const FSOps = {
    getNodeMode(parent, name, info) {
      return {
        permissions: 0o555, // read and execute but not write
        isDir: typeof info !== "number",
      };
    },
    setNodeAttributes(node, info, isDir) {
      node.modtime = TIMESTAMP;
      node.usedBytes = 0;
      if (info === undefined) {
        info = rootInfo;
      }
      if (isDir) {
        node.tree = info;
      } else {
        node.index = info;
        node.usedBytes = sizes[info];
      }
    },
    readdir(node) {
      return Array.from(node.tree.keys());
    },
    lookup(parent, name) {
      return parent.tree.get(name);
    },
    read(stream, position, buffer) {
      return MetadataReader.read(stream.node.index, position, buffer);
    },
  };

  return createReadonlyFS(FSOps, Module);
}
