
interface Reader {
  read: (offset: number, dest: Uint8Array) => number;
}

interface FSInfo {
  children: Map<string, FSInfo> | undefined;
  mode: number;
  type: string;
  modtime: number;
  size: number;
  path: string;
  name: string;
  parts: string[];
  contentsOffset?: number;
  reader?: Reader;
}

declare type MetadataFSInfo = Map<string, MetadataFSInfo>;

interface FS {
  mkdir: (dirname: string) => void;
  mkdirTree: (dirname: string) => void;
  writeFile: (fname: string, contents: Uint8Array, options: object) => void;
  mount: (fs: object, options: { info?: FSInfo }, path: string) => void;
  createNode: (parent: FSNode | null, name: string, mode: number) => FSNode;
  isFile: (mode: number) => boolean;
  genericErrors: Error[];
}

interface FSOps {
  getNodeMode: (parent: FSNode | null, name: string, info: any) => {
    permissions: number;
    isDir: boolean;
  };
  setNodeAttributes: (node: FSNode, info: any, isDir: boolean) => void;
  readdir: (node: FSNode) => string[];
  lookup: (parent: FSNode, name: string) => (FSInfo | MetadataFSInfo);
  read: (stream: FSStream, position: number, buffer: Uint8Array) => number;
}

interface MountOpts {
  opts: { info: FSInfo }
}

interface FSNodeOps {
  getattr: (node: FSNode) => FSAttr;
  readdir: (node: FSNode) => string[];
  lookup: (parent: FSNode, name: string) => FSNode;
}

interface FSStream {
  node: FSNode;
  position: number;
}

interface FSStreamOps {
  llseek: (stream: FSStream, offset: number, whence: number) => number;
  read: (stream: FSStream, buffer: Uint8Array, offset: number, length: number, position: number) => number;
}

interface FSNode {
  id: number;
  usedBytes: number;
  mode: number;
  modtime: number;
  node_ops: FSNodeOps;
  stream_ops: FSStreamOps;
  info: FSInfo;
  contentsOffset?: number;
  tree?: MetadataFSInfo;
  index?: number;
}

interface FSAttr {
  dev: number;
  ino: number;
  mode: number;
  nlink: number;
  uid: number;
  gid: number,
  rdev: number,
  size: number,
  atime: Date,
  mtime: Date,
  ctime: Date,
  blksize: number;
  blocks: number;
}

interface EmscriptenFS {
  mount: (mount: MountOpts) => FSNode;
  createNode: (parent: FSNode | null, name: string, info: FSInfo | MetadataFSInfo) => FSNode;
  node_ops: FSNodeOps;
  stream_ops: FSStreamOps;
}
