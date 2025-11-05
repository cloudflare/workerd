import { createReadonlyFS } from 'pyodide-internal:readOnlyFS';
import { PythonWorkersInternalError } from 'pyodide-internal:util';

const FSOps: FSOps<TarFSInfo> = {
  getNodeMode(_parent, _name, info) {
    return {
      permissions: info.mode,
      isDir: info.children !== undefined,
    };
  },
  setNodeAttributes(node, info, isDir) {
    node.info = info!;
    node.modtime = node.info.modtime;
    node.usedBytes = 0;
    if (!isDir) {
      node.contentsOffset = node.info.contentsOffset;
      node.usedBytes = node.info.size;
    }
  },
  readdir(node) {
    if (!node.info.children) {
      throw new PythonWorkersInternalError(
        'Trying to readdir from a non-dir node'
      );
    }
    return Array.from(node.info.children.keys());
  },
  lookup(parent, name) {
    if (!parent.info.children) {
      throw new PythonWorkersInternalError(
        'Trying to lookup from a non-dir node'
      );
    }
    return parent.info.children.get(name)!;
  },
  read(stream, position, buffer) {
    if (stream.node.contentsOffset == undefined) {
      throw new PythonWorkersInternalError('contentsOffset is undefined');
    }
    if (!stream.node.info.reader) {
      throw new PythonWorkersInternalError('reader is undefined');
    }
    return stream.node.info.reader.read(
      stream.node.contentsOffset + position,
      buffer
    );
  },
};

export function createTarFS(Module: Module): EmscriptenFS<TarFSInfo> {
  return createReadonlyFS(FSOps, Module);
}
