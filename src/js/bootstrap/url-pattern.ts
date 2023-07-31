export interface URLPatternURLPatternInit {
  protocol?: string;
  username?: string;
  password?: string;
  hostname?: string;
  port?: string;
  pathname?: string;
  search?: string;
  hash?: string;
  baseURL?: string;
}

export interface URLPatternURLPatternComponentResult {
  input: string;
  groups: Record<string, string>;
}

interface URLPatternURLPatternResult {
  inputs: (string | URLPatternURLPatternInit)[];
  protocol: URLPatternURLPatternComponentResult;
  username: URLPatternURLPatternComponentResult;
  password: URLPatternURLPatternComponentResult;
  hostname: URLPatternURLPatternComponentResult;
  port: URLPatternURLPatternComponentResult;
  pathname: URLPatternURLPatternComponentResult;
  search: URLPatternURLPatternComponentResult;
  hash: URLPatternURLPatternComponentResult;
}

type URLPatternComponent = string;

interface URLPatternComponents {
  // The collection of compiled patterns for each component of a URLPattern.
  protocol: URLPatternComponent;
  username: URLPatternComponent;
  password: URLPatternComponent;
  hostname: URLPatternComponent;
  port: URLPatternComponent;
  pathname: URLPatternComponent;
  search: URLPatternComponent;
  hash: URLPatternComponent;
};

export class URLPattern {
  private readonly components: URLPatternComponents;

  constructor(input?: string | URLPatternURLPatternInit, baseURL?: string) {
    this.components = init(input, baseURL);
  }
  get protocol(): string {
    throw new Error("NOT IMPLEMENTED" + this.components);
  }
  get username(): string {
    throw new Error("NOT IMPLEMENTED");
  }
  get password(): string {
    throw new Error("NOT IMPLEMENTED");
  }
  get hostname(): string {
    throw new Error("NOT IMPLEMENTED");
  }
  get port(): string {
    throw new Error("NOT IMPLEMENTED");
  }
  get pathname(): string {
    throw new Error("NOT IMPLEMENTED");
  }
  get search(): string {
    throw new Error("NOT IMPLEMENTED");
  }
  get hash(): string {
    throw new Error("NOT IMPLEMENTED");
  }
  test(_input?: string | URLPatternURLPatternInit, _baseURL?: string): boolean {
    throw new Error("NOT IMPLEMENTED");
  }
  exec(
    _input?: string | URLPatternURLPatternInit,
    _baseURL?: string
  ): URLPatternURLPatternResult | null {
    throw new Error("NOT IMPLEMENTED");
  }
}

function init(input?: string | URLPatternURLPatternInit, _baseURL?: string): URLPatternComponents {
  if (typeof input == "string") {
    throw new Error("init::string")
  } else {
    throw new Error("init::init")
  }
}
