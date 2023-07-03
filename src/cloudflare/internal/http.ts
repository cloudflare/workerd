interface Blob {

}

interface INativeRequest {
  method: string;
  url: string;

  readAllText(): Promise<String>;
}

class RequestImpl {
  public readonly method: string;
  public readonly url: string;

  constructor(readonly native: INativeRequest) {
    this.method = native.method;
    this.url = native.url;
  }

  public async blob(): Promise<Blob> {
    const native = this.native;

    return {
      async text(): Promise<String> {
        return native.readAllText();
      }
    };
  }
}

export function createRequest(native: INativeRequest) {
  return new RequestImpl(native);
}

export class Response {
  public body: string;
  public status: number;
  public statusText: string = "";

  constructor(body: string, options?: { status?: number }) {
    this.status = options?.status ?? 200;
    this.body = body;
  }
}
