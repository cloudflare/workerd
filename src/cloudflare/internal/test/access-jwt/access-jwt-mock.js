// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/**
 * Mock JWKS endpoint and JWT generator for testing Access JWT validation.
 *
 * This mock generates real RSA key pairs and signs JWTs that can be validated
 * by the actual validation code.
 */

// Store generated key pairs by team domain
const keyPairs = new Map();

// Track request counts for retry testing
let requestCount = 0;

// Per-team failure configuration: teamDomain -> { failCount, requestCount }
const failureConfig = new Map();

/**
 * Base64url encode a buffer or string.
 */
function base64UrlEncode(data) {
  let bytes;
  if (typeof data === 'string') {
    bytes = new TextEncoder().encode(data);
  } else {
    bytes = new Uint8Array(data);
  }
  return bytes.toBase64({ alphabet: 'base64url', omitPadding: true });
}

/**
 * Generate an RSA key pair for signing JWTs.
 */
async function generateKeyPair() {
  const keyPair = await crypto.subtle.generateKey(
    {
      name: 'RSASSA-PKCS1-v1_5',
      modulusLength: 2048,
      publicExponent: new Uint8Array([1, 0, 1]),
      hash: 'SHA-256',
    },
    true,
    ['sign', 'verify']
  );

  // Export public key as JWK for the JWKS endpoint
  const publicJwk = await crypto.subtle.exportKey('jwk', keyPair.publicKey);

  // Generate a kid
  const kid = crypto.randomUUID();
  publicJwk.kid = kid;
  publicJwk.alg = 'RS256';
  publicJwk.use = 'sig';

  return {
    privateKey: keyPair.privateKey,
    publicKey: keyPair.publicKey,
    publicJwk,
    kid,
  };
}

/**
 * Get or create a key pair for a team domain.
 */
async function getKeyPair(teamDomain) {
  let value = keyPairs.get(teamDomain);
  if (value == null) {
    value = await generateKeyPair();
    keyPairs.set(teamDomain, value);
  }
  return value;
}

/**
 * Create a signed JWT.
 */
async function createJwt(teamDomain, claims, options = {}) {
  const keyPair = await getKeyPair(teamDomain);

  const header = {
    alg: options.alg || 'RS256',
    typ: 'JWT',
    kid: options.kid || keyPair.kid,
  };

  const now = Math.floor(Date.now() / 1000);
  const payload = {
    iss: `https://${teamDomain}.cloudflareaccess.com`,
    sub: claims.sub || 'test-user-id',
    aud: claims.aud || ['test-audience'],
    email: claims.email || 'test@example.com',
    iat: claims.iat || now,
    exp: claims.exp || now + 3600, // 1 hour from now
    ...claims,
  };

  const headerB64 = base64UrlEncode(JSON.stringify(header));
  const payloadB64 = base64UrlEncode(JSON.stringify(payload));
  const signedPart = `${headerB64}.${payloadB64}`;

  // Sign with the private key (or use a different key if specified)
  const signingKey = options.signingKey || keyPair.privateKey;
  const encoder = new TextEncoder();
  const signature = await crypto.subtle.sign(
    { name: 'RSASSA-PKCS1-v1_5' },
    signingKey,
    encoder.encode(signedPart)
  );

  const signatureB64 = base64UrlEncode(signature);

  return `${signedPart}.${signatureB64}`;
}

/**
 * Handle requests to the mock JWKS endpoint.
 */
async function handleJwksRequest(teamDomain) {
  requestCount++;

  // Check for per-team failure configuration
  const teamFailConfig = failureConfig.get(teamDomain);
  if (teamFailConfig) {
    teamFailConfig.requestCount = (teamFailConfig.requestCount || 0) + 1;
    if (teamFailConfig.requestCount <= teamFailConfig.failCount) {
      return new Response('Service Unavailable', { status: 503 });
    }
  }

  const keyPair = await getKeyPair(teamDomain);

  const jwks = {
    keys: [keyPair.publicJwk],
  };

  return Response.json(jwks);
}

export default {
  async fetch(request) {
    const url = new URL(request.url);

    // Reset all state
    if (url.pathname === '/_test/reset') {
      requestCount = 0;
      failureConfig.clear();
      keyPairs.clear();
      return new Response('OK');
    }

    // Reset just the request count (for caching tests)
    if (url.pathname === '/_test/reset-count') {
      requestCount = 0;
      return new Response('OK');
    }

    // Get request count for assertions
    if (url.pathname === '/_test/request-count') {
      return Response.json({ count: requestCount });
    }

    // Configure per-team failure behavior
    if (url.pathname === '/_test/configure-failure') {
      const config = await request.json();
      const teamDomain = config.teamDomain;
      if (teamDomain) {
        failureConfig.set(teamDomain, {
          failCount: config.failCount || 0,
          requestCount: 0,
        });
      }
      return new Response('OK');
    }

    // Generate a JWT for testing
    if (url.pathname === '/_test/generate-jwt') {
      const body = await request.json();
      const teamDomain = body.teamDomain || 'test-team';
      const claims = body.claims || {};
      const options = body.options || {};

      // If requested, use a different key for signing (to test invalid signatures)
      if (options.useWrongKey) {
        const wrongKeyPair = await generateKeyPair();
        options.signingKey = wrongKeyPair.privateKey;
      }

      const jwt = await createJwt(teamDomain, claims, options);
      return new Response(JSON.stringify({ jwt }), {
        headers: { 'Content-Type': 'application/json' },
      });
    }

    // JWKS endpoint - matches the pattern /<team>.cloudflareaccess.com/cdn-cgi/access/certs
    // In our test setup, we route all requests through this mock
    const jwksMatch = url.pathname.match(
      /^\/([^/]+)\.cloudflareaccess\.com\/cdn-cgi\/access\/certs$/
    );
    if (jwksMatch) {
      const teamDomain = jwksMatch[1];
      return handleJwksRequest(teamDomain);
    }

    // Also handle direct /cdn-cgi/access/certs requests with host header
    if (url.pathname === '/cdn-cgi/access/certs') {
      const host = request.headers.get('host') || '';
      const teamMatch = host.match(/^([^.]+)\.cloudflareaccess\.com$/);
      if (teamMatch) {
        return handleJwksRequest(teamMatch[1]);
      }
    }

    // Return 404 for unknown paths
    if (url.pathname.includes('/cdn-cgi/access/certs')) {
      // Unknown team - return 404
      return new Response('Not Found', { status: 404 });
    }

    return new Response('Not Found', { status: 404 });
  },
};
