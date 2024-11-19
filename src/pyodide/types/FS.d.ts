interface Reader {
  read: (offset: number, dest: Uint8Array) => number;
}

interface TarFSInfo {
  children: Map<string, TarFSInfo> | undefined;
  mode: number;
  type: string;
  modtime: number;
  size: number;
  path: string;
  name: string;
  parts: string[];
  contentsOffset?: number;
  reader: Reader | null;
}

declare type MetadataDirInfo = Map<string, MetadataDirInfo>;
declare type MetadataFSInfo = MetadataDirInfo | number; // file infos are numbers and dir infos are maps

interface FS {
  mkdir: (dirname: string) => void;
  mkdirTree: (dirname: string) => void;
  writeFile: (fname: string, contents: Uint8Array, options: object) => void;
  mount<Info>(fs: object, options: { info?: Info }, path: string): void;
  createNode<Info>(
    parent: FSNode<Info> | null,
    name: string,
    mode: number
  ): FSNode<Info>;
  isFile: (mode: number) => boolean;
  readdir: (path: string) => string[];
  genericErrors: Error[];
}

interface FSOps<Info> {
  getNodeMode: (
    parent: FSNode<Info> | null,
    name: string,
    info: Info
  ) => {
    permissions: number;
    isDir: boolean;
  };
  setNodeAttributes: (node: FSNode<Info>, info: Info, isDir: boolean) => void;
  readdir: (node: FSNode<Info>) => string[];
  lookup: (parent: FSNode<Info>, name: string) => Info;
  read: (
    stream: FSStream<Info>,
    position: number,
    buffer: Uint8Array
  ) => number;
}

interface MountOpts<Info> {
  opts: { info: Info };
}

interface FSNodeOps<Info> {
  getattr: (node: FSNode<Info>) => FSAttr;
  readdir: (node: FSNode<Info>) => string[];
  lookup: (parent: FSNode<Info>, name: string) => FSNode<Info>;
}

interface FSStream<Info> {
  node: FSNode<Info>;
  position: number;
}

interface FSStreamOps<Info> {
  llseek: (stream: FSStream<Info>, offset: number, whence: number) => number;
  read: (
    stream: FSStream<Info>,
    buffer: Uint8Array,
    offset: number,
    length: number,
    position: number
  ) => number;
}

interface FSNode<Info> {
  id: number;
  usedBytes: number;
  mode: number;
  modtime: number;
  node_ops: FSNodeOps<Info>;
  stream_ops: FSStreamOps<Info>;
  info: Info;
  contentsOffset?: number;
  tree?: MetadataDirInfo;
  index?: number;
}

interface FSAttr {
  dev: number;
  ino: number;
  mode: number;
  nlink: number;
  uid: number;
  gid: number;
  rdev: number;
  size: number;
  atime: Date;
  mtime: Date;
  ctime: Date;
  blksize: number;
  blocks: number;
}

interface EmscriptenFS<Info> {
  mount: (mount: MountOpts<Info>) => FSNode<Info>;
  createNode: (
    parent: FSNode<Info> | null,
    name: string,
    info: Info
  ) => FSNode<Info>;
  node_ops: FSNodeOps<Info>;
  stream_ops: FSStreamOps<Info>;
}
