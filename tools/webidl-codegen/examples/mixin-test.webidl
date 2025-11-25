// Test for mixin support

interface mixin Body {
  readonly attribute ReadableStream? body;
  readonly attribute boolean bodyUsed;
  Promise<ArrayBuffer> arrayBuffer();
  Promise<Blob> blob();
  Promise<any> json();
  Promise<USVString> text();
};

interface Request {
  constructor(USVString url);
  readonly attribute USVString url;
  readonly attribute ByteString method;
};
Request includes Body;

interface Response {
  constructor(optional any body);
  readonly attribute unsigned short status;
  readonly attribute ByteString statusText;
};
Response includes Body;
