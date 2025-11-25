// Test case for multiple mixin inheritance

interface mixin Body {
  readonly attribute any body;
  readonly attribute boolean bodyUsed;
  Promise<ArrayBuffer> arrayBuffer();
  Promise<Blob> blob();
  Promise<any> json();
  Promise<USVString> text();
};

interface mixin Headers {
  void append(ByteString name, ByteString value);
  void delete(ByteString name);
  ByteString? get(ByteString name);
  boolean has(ByteString name);
};

[Exposed=ServiceWorker]
interface Request {
  constructor(USVString url);
  readonly attribute USVString url;
  readonly attribute ByteString method;
};

Request includes Body;
Request includes Headers;
