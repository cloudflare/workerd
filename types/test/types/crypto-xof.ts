export async function xofDigests(crypto: Crypto): Promise<void> {
  const cshake: SubtleCryptoCShakeParams = {
    name: 'cSHAKE256',
    outputLength: 256,
    functionName: new Uint8Array(),
    customization: new ArrayBuffer(0),
  };
  const turbo: SubtleCryptoTurboShakeParams = {
    name: 'TurboSHAKE256',
    outputLength: 512,
    domainSeparation: 31,
  };
  await crypto.subtle.digest(cshake, new Uint8Array());
  await crypto.subtle.digest(turbo, new ArrayBuffer(0));
  SubtleCrypto.supports('digest', {
    name: 'cSHAKE256',
    outputLength: 256,
    customization: new Uint8Array(),
  });
  SubtleCrypto.supports('digest', {
    name: 'TurboSHAKE256',
    outputLength: 512,
    domainSeparation: 31,
  });

  // @ts-expect-error XOF parameter dictionaries require an output length.
  const missingLength: SubtleCryptoCShakeParams = { name: 'cSHAKE256' };
  void missingLength;
  const wrongCustomization: SubtleCryptoCShakeParams = {
    name: 'cSHAKE256',
    outputLength: 256,
    // @ts-expect-error cSHAKE customization is a BufferSource.
    customization: '',
  };
  void wrongCustomization;
}
