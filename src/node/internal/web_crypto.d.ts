// Just enough of web crypto api to write the code in node-compat

declare namespace crypto {
  function getRandomValues<T extends ArrayBufferView | null>(array: T): T;
  function randomUUID(): string;

  const subtle: SubtleCrypto;
}

type SubtleCrypto = unknown;

type CryptoKey = unknown;

declare const CryptoKey: {
  prototype: CryptoKey;
  new(): CryptoKey;
};
