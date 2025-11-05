// Allow {CLIENT_RENEG_LIMIT} client-initiated session renegotiations
// every {CLIENT_RENEG_WINDOW} seconds. An error event is emitted if more
// renegotiations are seen. The settings are applied to all remote client
// connections.
export const CLIENT_RENEG_LIMIT = 3;
export const CLIENT_RENEG_WINDOW = 600;
export const DEFAULT_CIPHERS = '';
export const DEFAULT_ECDH_CURVE = 'auto';
export const DEFAULT_MIN_VERSION = 'TLSv1.2';
export const DEFAULT_MAX_VERSION = 'TLSv1.3';
export const rootCertificates = [];
