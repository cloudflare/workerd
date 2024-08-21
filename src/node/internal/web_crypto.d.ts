// Just enough of web crypto api to write the code in node-compat
type SubtleCrypto = unknown;

type CryptoKey = unknown;

declare const CryptoKey: {
  prototype: CryptoKey;
  new (): CryptoKey;
};
