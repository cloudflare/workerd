interface FSInfo {
  children: Map<string, FSInfo> | undefined;
  mode: number;
  type: number;
  modtime: number;
  size: number;
  path: string;
  name: string;
  parts: string[];
  contentsOffset?: number;
}

interface FS {
  mkdir: (dirname: string) => void;
  mkdirTree: (dirname: string) => void;
  writeFile: (fname: string, contents: Uint8Array, options: object) => void;
  mount: (fs: object, options: { info?: FSInfo }, path: string) => void;
}
