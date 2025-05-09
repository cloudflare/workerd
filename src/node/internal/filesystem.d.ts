export enum NodeType {
  FILE = 0,
  DIRECTORY = 1,
}

export class FileHandle {
  public close(): void;
}

export class Node {
  public getStat(): {
    path: string;
    name: string;
    createdAt: number;
    modifiedAt: number;
    type: NodeType;
  };

  public getFd(): FileHandle | undefined;

  public readonly readable: boolean;
  public readonly writable: boolean;
  public readonly asyncOnly: boolean;
  public readonly syncOnly: boolean;
}

export function open(pathLike: string | URL | Buffer): Node | undefined;
