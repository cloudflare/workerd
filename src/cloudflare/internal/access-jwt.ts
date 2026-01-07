// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/**
 * Cloudflare Access JWT validation for Workers.
 *
 * Validates JWTs from the `cf-access-jwt-assertion` header against
 * Cloudflare Access's public keys.
 */

const ACCESS_JWT_HEADER = 'cf-access-jwt-assertion';
const CLOCK_SKEW_SECONDS = 60;
const CACHE_TTL_MS = 60 * 60 * 1000; // 1 hour
const RETRY_DELAY_MS = 5000;
const MAX_RETRIES = 3;

declare global {
  // eslint-disable-next-line no-var
  var scheduler: {
    wait: (delay: number, options?: { signal?: AbortSignal }) => Promise<void>;
  };
}

export type AccessJwtErrorCode =
  | 'ERR_JWT_MISSING'
  | 'ERR_JWT_MALFORMED'
  | 'ERR_JWT_INVALID_SIGNATURE'
  | 'ERR_JWT_EXPIRED'
  | 'ERR_JWT_NOT_YET_VALID'
  | 'ERR_JWT_AUDIENCE_MISMATCH'
  | 'ERR_JWT_ISSUER_MISMATCH'
  | 'ERR_JWKS_FETCH_FAILED'
  | 'ERR_JWKS_NO_MATCHING_KEY'
  | 'ERR_INVALID_TEAM_DOMAIN'
  | 'ERR_INVALID_AUDIENCE';

export class AccessJwtError extends Error {
  readonly code: AccessJwtErrorCode;

  constructor(code: AccessJwtErrorCode, message: string) {
    super(message);
    this.name = 'AccessJwtError';
    this.code = code;
  }
}

export interface AccessJwtPayload {
  aud: string[];
  email?: string;
  exp: number;
  iat: number;
  nbf?: number;
  iss: string;
  sub: string;
  [key: string]: unknown;
}

interface JwtHeader {
  alg: string;
  kid: string;
  typ?: string;
}

interface JwkKey {
  kid: string;
  kty: string;
  alg: string;
  use: string;
  e: string;
  n: string;
}

interface JwkSet {
  keys: JwkKey[];
}

interface CacheEntry {
  jwks: JwkSet;
  fetchedAt: number;
}

// Module-level cache persists across requests within the same isolate
const jwksCache = new Map<string, CacheEntry>();

// Module-level encoder/decoder for reuse
const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();

/**
 * Normalize team domain to the full cloudflareaccess.com hostname.
 * Accepts both "mycompany" and "mycompany.cloudflareaccess.com".
 */
function normalizeTeamDomain(teamDomain: string): string {
  const trimmed = teamDomain.trim().toLowerCase();
  if (trimmed.endsWith('.cloudflareaccess.com')) {
    return trimmed;
  }
  return `${trimmed}.cloudflareaccess.com`;
}

// Uint8Array.fromBase64/toBase64 available at runtime but not in TS types yet
type Uint8ArrayWithBase64 = Uint8Array & {
  toBase64(options?: {
    alphabet?: 'base64' | 'base64url' | undefined;
    omitPadding?: boolean | undefined;
  }): string;
};
interface Uint8ArrayConstructorWithBase64 {
  new (length: number): Uint8ArrayWithBase64;
  fromBase64(
    string: string,
    options?: {
      alphabet?: 'base64' | 'base64url' | undefined;
      lastChunkHandling?:
        | 'loose'
        | 'strict'
        | 'stop-before-partial'
        | undefined;
    }
  ): Uint8ArrayWithBase64;
}
const Uint8ArrayWithBase64 =
  Uint8Array as unknown as Uint8ArrayConstructorWithBase64;

type FixedLengthArray<
  T,
  N extends number,
  A extends T[] = [],
> = A['length'] extends N ? A : FixedLengthArray<T, N, [T, ...A]>;

/**
 * Parse a JWT into its components without verifying.
 */
function parseJwt(token: string): {
  header: JwtHeader;
  payload: AccessJwtPayload;
  signature: ArrayBuffer;
  signedPart: string;
} {
  const parts = token.split('.');
  if (parts.length !== 3) {
    throw new AccessJwtError(
      'ERR_JWT_MALFORMED',
      `JWT must have 3 parts, got ${parts.length}`
    );
  }

  const [headerB64, payloadB64, signatureB64] = parts as FixedLengthArray<
    string,
    3
  >;

  try {
    const header = JSON.parse(
      textDecoder.decode(
        Uint8ArrayWithBase64.fromBase64(headerB64, { alphabet: 'base64url' })
      )
    ) as JwtHeader;

    const payload = JSON.parse(
      textDecoder.decode(
        Uint8ArrayWithBase64.fromBase64(payloadB64, { alphabet: 'base64url' })
      )
    ) as AccessJwtPayload;

    const signature = Uint8ArrayWithBase64.fromBase64(signatureB64, {
      alphabet: 'base64url',
    }).buffer as ArrayBuffer;

    // The signed part is the header and payload joined by a dot (not decoded)
    const signedPart = `${headerB64}.${payloadB64}`;

    return { header, payload, signature, signedPart };
  } catch (e) {
    if (e instanceof AccessJwtError) {
      throw e;
    }
    throw new AccessJwtError(
      'ERR_JWT_MALFORMED',
      `Failed to parse JWT: ${Error.isError(e) ? e.message : 'unknown error'}`
    );
  }
}

/**
 * Sleep for the specified duration using scheduler.wait.
 */
function sleep(ms: number): Promise<void> {
  return globalThis.scheduler.wait(ms);
}

/**
 * Fetch JWKS from Cloudflare Access with retry logic for 5xx errors.
 */
async function fetchJwks(normalizedDomain: string): Promise<JwkSet> {
  const url = `https://${normalizedDomain}/cdn-cgi/access/certs`;

  let lastError: Error | null = null;

  for (let attempt = 0; attempt < MAX_RETRIES; attempt++) {
    if (attempt > 0) {
      await sleep(RETRY_DELAY_MS);
    }

    try {
      const res = await fetch(url);

      if (res.ok) {
        const data = (await res.json()) as JwkSet;
        return data;
      }

      // Don't retry client errors (4xx)
      if (res.status < 500) {
        throw new AccessJwtError(
          'ERR_JWKS_FETCH_FAILED',
          `Failed to fetch JWKS: ${res.status} ${res.statusText}`
        );
      }

      // 5xx errors: continue to retry
      lastError = new Error(`HTTP ${res.status} ${res.statusText}`);
    } catch (e) {
      if (e instanceof AccessJwtError) {
        throw e;
      }
      lastError = Error.isError(e)
        ? e
        : new Error('Unknown fetch error', { cause: e });
    }
  }

  throw new AccessJwtError(
    'ERR_JWKS_FETCH_FAILED',
    `Failed to fetch JWKS after ${MAX_RETRIES} attempts: ${lastError?.message ?? 'unknown error'}`
  );
}

/**
 * Get JWKS with caching. Returns cached value if still valid,
 * otherwise fetches fresh JWKS.
 */
async function getJwks(
  normalizedDomain: string,
  forceRefresh = false
): Promise<JwkSet> {
  if (!forceRefresh) {
    const cached = jwksCache.get(normalizedDomain);
    if (cached && Date.now() - cached.fetchedAt < CACHE_TTL_MS) {
      return cached.jwks;
    }
  }

  const jwks = await fetchJwks(normalizedDomain);
  jwksCache.set(normalizedDomain, { jwks, fetchedAt: Date.now() });
  return jwks;
}

/**
 * Find a key in the JWKS matching the given kid.
 */
function findKey(jwks: JwkSet, kid: string): JwkKey | undefined {
  return jwks.keys.find((key) => key.kid === kid);
}

/**
 * Import a JWK as a CryptoKey for signature verification.
 */
async function importKey(jwk: JwkKey): Promise<CryptoKey> {
  return crypto.subtle.importKey(
    'jwk',
    jwk,
    {
      name: 'RSASSA-PKCS1-v1_5',
      hash: 'SHA-256',
    },
    false,
    ['verify']
  );
}

/**
 * Verify the JWT signature using the provided key.
 */
async function verifySignature(
  key: CryptoKey,
  signature: ArrayBuffer,
  signedPart: string
): Promise<boolean> {
  const data = textEncoder.encode(signedPart);

  return crypto.subtle.verify(
    { name: 'RSASSA-PKCS1-v1_5' },
    key,
    signature,
    data
  );
}

/**
 * Validate JWT claims (audience, expiration, issuer).
 */
function validateClaims(
  payload: AccessJwtPayload,
  normalizedDomain: string,
  audience: string
): void {
  // Validate issuer
  const expectedIssuer = `https://${normalizedDomain}`;
  if (payload.iss !== expectedIssuer) {
    throw new AccessJwtError(
      'ERR_JWT_ISSUER_MISMATCH',
      `Expected issuer "${expectedIssuer}", got "${payload.iss}"`
    );
  }

  // Validate expiration with clock skew tolerance
  const now = Math.floor(Date.now() / 1000);
  if (payload.exp < now - CLOCK_SKEW_SECONDS) {
    throw new AccessJwtError(
      'ERR_JWT_EXPIRED',
      `Token expired at ${new Date(payload.exp * 1000).toISOString()}`
    );
  }

  // Validate not-before if present (RFC 7519)
  if (payload.nbf !== undefined && payload.nbf > now + CLOCK_SKEW_SECONDS) {
    throw new AccessJwtError(
      'ERR_JWT_NOT_YET_VALID',
      `Token not valid until ${new Date(payload.nbf * 1000).toISOString()}`
    );
  }

  // Validate audience
  // aud can be a string or array in the JWT spec
  const audArray = Array.isArray(payload.aud) ? payload.aud : [payload.aud];
  if (!audArray.includes(audience)) {
    throw new AccessJwtError(
      'ERR_JWT_AUDIENCE_MISMATCH',
      `Expected audience "${audience}", got "${audArray.join(', ')}"`
    );
  }
}

/**
 * Internal validation logic, separated for retry-on-missing-kid behavior.
 */
async function validateJwtInternal(
  token: string,
  normalizedDomain: string,
  audience: string,
  forceRefreshJwks: boolean
): Promise<AccessJwtPayload> {
  const { header, payload, signature, signedPart } = parseJwt(token);

  // Cloudflare Access uses RS256
  if (header.alg !== 'RS256') {
    throw new AccessJwtError(
      'ERR_JWT_MALFORMED',
      `Unsupported algorithm "${header.alg}", expected RS256`
    );
  }

  // Fetch JWKS and find matching key
  const jwks = await getJwks(normalizedDomain, forceRefreshJwks);
  const jwk = findKey(jwks, header.kid);

  if (!jwk) {
    throw new AccessJwtError(
      'ERR_JWKS_NO_MATCHING_KEY',
      `No key found with kid "${header.kid}"`
    );
  }

  // Verify signature
  const cryptoKey = await importKey(jwk);
  const isValid = await verifySignature(cryptoKey, signature, signedPart);

  if (!isValid) {
    throw new AccessJwtError(
      'ERR_JWT_INVALID_SIGNATURE',
      'JWT signature verification failed'
    );
  }

  // Validate claims
  validateClaims(payload, normalizedDomain, audience);

  return payload;
}

/**
 * Validates a Cloudflare Access JWT from an incoming request.
 *
 * @param req - The incoming Request containing the cf-access-jwt-assertion header
 * @param teamDomain - The Cloudflare One team domain (e.g., "mycompany" or "mycompany.cloudflareaccess.com")
 * @param audience - The Application Audience (AUD) tag
 * @throws {AccessJwtError} If validation fails for any reason
 * @returns The decoded JWT payload on success
 */
export async function validateAccessJwt(
  req: Request,
  teamDomain: string,
  audience: string
): Promise<AccessJwtPayload> {
  // Validate inputs
  if (!teamDomain || teamDomain.trim() === '') {
    throw new AccessJwtError(
      'ERR_INVALID_TEAM_DOMAIN',
      'Team domain cannot be empty'
    );
  }

  if (!audience || audience.trim() === '') {
    throw new AccessJwtError(
      'ERR_INVALID_AUDIENCE',
      'Audience cannot be empty'
    );
  }

  // Get token from header
  const token = req.headers.get(ACCESS_JWT_HEADER);
  if (!token) {
    throw new AccessJwtError(
      'ERR_JWT_MISSING',
      `Missing ${ACCESS_JWT_HEADER} header`
    );
  }

  const normalizedDomain = normalizeTeamDomain(teamDomain);

  try {
    // First attempt with potentially cached JWKS
    return await validateJwtInternal(token, normalizedDomain, audience, false);
  } catch (e) {
    // If we got a missing key error, try once more with fresh JWKS
    // This handles the edge case of key rotation with stale cache
    if (e instanceof AccessJwtError && e.code === 'ERR_JWKS_NO_MATCHING_KEY') {
      return await validateJwtInternal(token, normalizedDomain, audience, true);
    }
    throw e;
  }
}

// For testing purposes - allows clearing the cache
export function _clearJwksCache(): void {
  jwksCache.clear();
}
