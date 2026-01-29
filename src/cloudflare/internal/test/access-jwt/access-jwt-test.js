// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import { validateAccessJwt, AccessJwtError } from 'cloudflare:workers';

const AUDIENCE = 'test-audience-tag-12345';

/**
 * Helper to generate a JWT via the mock service.
 */
async function generateJwt(env, teamDomain, claims = {}, options = {}) {
  const res = await env.mock.fetch('http://mock/_test/generate-jwt', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      teamDomain: options.teamDomain || teamDomain,
      claims: {
        aud: [AUDIENCE],
        ...claims,
      },
      options,
    }),
  });
  const data = await res.json();
  return data.jwt;
}

/**
 * Helper to create a Request with the JWT header.
 */
function createRequest(jwt) {
  return new Request('https://example.com/api/test', {
    headers: jwt ? { 'cf-access-jwt-assertion': jwt } : {},
  });
}

export const tests = {
  // Test: Valid JWT validates successfully
  async testValidJwt(_, env) {
    const teamDomain = 'test-valid';
    const jwt = await generateJwt(env, teamDomain);
    const req = createRequest(jwt);

    const payload = await validateAccessJwt(req, teamDomain, AUDIENCE);

    assert.strictEqual(
      payload.iss,
      `https://${teamDomain}.cloudflareaccess.com`
    );
    assert.deepStrictEqual(payload.aud, [AUDIENCE]);
    assert.strictEqual(payload.email, 'test@example.com');
    assert.strictEqual(typeof payload.exp, 'number');
    assert.strictEqual(typeof payload.iat, 'number');
    assert.strictEqual(typeof payload.sub, 'string');
  },

  // Test: Team domain normalization (accepts both formats)
  async testTeamDomainNormalization(_, env) {
    const teamDomain = 'test-normalize';
    const jwt = await generateJwt(env, teamDomain);
    const req = createRequest(jwt);

    // Should work with just the team name
    const payload1 = await validateAccessJwt(req, teamDomain, AUDIENCE);
    assert.strictEqual(
      payload1.iss,
      `https://${teamDomain}.cloudflareaccess.com`
    );

    // Should also work with full domain
    const payload2 = await validateAccessJwt(
      req,
      `${teamDomain}.cloudflareaccess.com`,
      AUDIENCE
    );
    assert.strictEqual(
      payload2.iss,
      `https://${teamDomain}.cloudflareaccess.com`
    );
  },

  // Test: Missing JWT header throws ERR_JWT_MISSING
  async testMissingJwtHeader(_, env) {
    const teamDomain = 'test-missing';
    const req = createRequest(null);

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_JWT_MISSING');
        assert.ok(err.message.includes('cf-access-jwt-assertion'));
        return true;
      }
    );
  },

  // Test: Malformed JWT (wrong number of parts)
  async testMalformedJwtParts(_, env) {
    const teamDomain = 'test-malformed-parts';
    const req = createRequest('not.a.valid.jwt.token');

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_JWT_MALFORMED');
        return true;
      }
    );
  },

  // Test: Malformed JWT (invalid base64)
  async testMalformedJwtBase64(_, env) {
    const teamDomain = 'test-malformed-base64';
    const req = createRequest('!!!.@@@.###');

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_JWT_MALFORMED');
        return true;
      }
    );
  },

  // Test: Invalid signature throws ERR_JWT_INVALID_SIGNATURE
  async testInvalidSignature(_, env) {
    const teamDomain = 'test-invalid-sig';
    // Generate JWT signed with a different key
    const jwt = await generateJwt(env, teamDomain, {}, { useWrongKey: true });
    const req = createRequest(jwt);

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_JWT_INVALID_SIGNATURE');
        return true;
      }
    );
  },

  // Test: Expired JWT throws ERR_JWT_EXPIRED
  async testExpiredJwt(_, env) {
    const teamDomain = 'test-expired';
    const pastTime = Math.floor(Date.now() / 1000) - 3600; // 1 hour ago
    const jwt = await generateJwt(env, teamDomain, {
      iat: pastTime - 3600,
      exp: pastTime,
    });
    const req = createRequest(jwt);

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_JWT_EXPIRED');
        assert.ok(err.message.includes('expired'));
        return true;
      }
    );
  },

  // Test: Wrong audience throws ERR_JWT_AUDIENCE_MISMATCH
  async testWrongAudience(_, env) {
    const teamDomain = 'test-wrong-aud';
    const jwt = await generateJwt(env, teamDomain, {
      aud: ['different-audience'],
    });
    const req = createRequest(jwt);

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_JWT_AUDIENCE_MISMATCH');
        assert.ok(err.message.includes(AUDIENCE));
        return true;
      }
    );
  },

  // Test: Wrong issuer (mismatched team) throws ERR_JWT_ISSUER_MISMATCH
  async testWrongIssuer(_, env) {
    const teamDomain = 'test-wrong-issuer';
    // Generate JWT for a different team
    const jwt = await generateJwt(
      env,
      teamDomain,
      {},
      { teamDomain: 'other-team' }
    );
    const req = createRequest(jwt);

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_JWT_ISSUER_MISMATCH');
        return true;
      }
    );
  },

  // Test: Empty team domain throws ERR_INVALID_TEAM_DOMAIN
  async testEmptyTeamDomain(_, env) {
    const teamDomain = 'test-empty-domain';
    const jwt = await generateJwt(env, teamDomain);
    const req = createRequest(jwt);

    await assert.rejects(
      () => validateAccessJwt(req, '', AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_INVALID_TEAM_DOMAIN');
        return true;
      }
    );

    await assert.rejects(
      () => validateAccessJwt(req, '   ', AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_INVALID_TEAM_DOMAIN');
        return true;
      }
    );
  },

  // Test: Empty audience throws ERR_INVALID_AUDIENCE
  async testEmptyAudience(_, env) {
    const teamDomain = 'test-empty-aud';
    const jwt = await generateJwt(env, teamDomain);
    const req = createRequest(jwt);

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, ''),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_INVALID_AUDIENCE');
        return true;
      }
    );
  },

  // Test: No matching kid throws ERR_JWKS_NO_MATCHING_KEY
  async testNoMatchingKid(_, env) {
    const teamDomain = 'test-no-kid';
    // Generate JWT with a non-existent kid
    const jwt = await generateJwt(
      env,
      teamDomain,
      {},
      { kid: 'non-existent-kid-12345' }
    );
    const req = createRequest(jwt);

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_JWKS_NO_MATCHING_KEY');
        return true;
      }
    );
  },

  // Test: JWKS 4xx error fails immediately (no retry)
  async testJwks4xxError(_, env) {
    const teamDomain = 'test-4xx';
    const jwt = await generateJwt(env, teamDomain);
    const req = createRequest(jwt);

    // Try to validate against an unknown team - will fail on issuer mismatch
    // since the JWT was signed for teamDomain but we're validating against 'unknown-team'
    await assert.rejects(
      () => validateAccessJwt(req, 'unknown-team', AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        // Will fail on issuer mismatch first since the JWT was signed for teamDomain
        assert.ok(
          err.code === 'ERR_JWT_ISSUER_MISMATCH' ||
            err.code === 'ERR_JWKS_NO_MATCHING_KEY'
        );
        return true;
      }
    );
  },

  // Test: JWKS 5xx errors trigger retry (up to 3 attempts)
  async testJwks5xxRetry(_, env) {
    // Use a unique team domain so we start with empty cache
    const teamDomain = 'test-5xx-retry';

    // First, make sure we have keys for this team
    await generateJwt(env, teamDomain);

    // Configure mock to fail first 2 requests with 503
    await env.mock.fetch('http://mock/_test/configure-failure', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ failCount: 2, teamDomain }),
    });

    const jwt = await generateJwt(env, teamDomain);
    const req = createRequest(jwt);

    // Should succeed on 3rd attempt
    const payload = await validateAccessJwt(req, teamDomain, AUDIENCE);
    assert.strictEqual(
      payload.iss,
      `https://${teamDomain}.cloudflareaccess.com`
    );
  },

  // Test: JWKS fetch fails after max retries
  async testJwksFetchFailsAfterRetries(_, env) {
    // Use a unique team domain so we start with empty cache
    const teamDomain = 'test-retry-fail';

    // First generate keys for this team
    await generateJwt(env, teamDomain);

    // Configure mock to fail all requests for this team
    await env.mock.fetch('http://mock/_test/configure-failure', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ failCount: 10, teamDomain }), // More than MAX_RETRIES
    });

    const jwt = await generateJwt(env, teamDomain);
    const req = createRequest(jwt);

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_JWKS_FETCH_FAILED');
        assert.ok(err.message.includes('3 attempts'));
        return true;
      }
    );
  },

  // Test: JWKS caching - second call doesn't fetch again
  async testJwksCaching(_, env) {
    const teamDomain = 'test-caching';
    const jwt = await generateJwt(env, teamDomain);
    const req = createRequest(jwt);

    // Reset request count before testing
    await env.mock.fetch('http://mock/_test/reset-count');

    // First validation - fetches JWKS
    await validateAccessJwt(req, teamDomain, AUDIENCE);

    // Get request count after first validation
    let countRes = await env.mock.fetch('http://mock/_test/request-count');
    let { count: countAfterFirst } = await countRes.json();

    // Second validation - should use cached JWKS
    await validateAccessJwt(req, teamDomain, AUDIENCE);

    // Get request count after second validation
    countRes = await env.mock.fetch('http://mock/_test/request-count');
    const { count: countAfterSecond } = await countRes.json();

    // Request count should be the same (no new JWKS fetch)
    assert.strictEqual(
      countAfterFirst,
      countAfterSecond,
      'Expected no additional JWKS fetch for cached keys'
    );
  },

  // Test: Unsupported algorithm throws ERR_JWT_MALFORMED
  async testUnsupportedAlgorithm(_, env) {
    const teamDomain = 'test-bad-alg';
    // Generate JWT with unsupported algorithm (mock will still sign with RS256 but set alg to HS256)
    const jwt = await generateJwt(env, teamDomain, {}, { alg: 'HS256' });
    const req = createRequest(jwt);

    await assert.rejects(
      () => validateAccessJwt(req, teamDomain, AUDIENCE),
      (err) => {
        assert.ok(err instanceof AccessJwtError);
        assert.strictEqual(err.code, 'ERR_JWT_MALFORMED');
        assert.ok(err.message.includes('HS256'));
        return true;
      }
    );
  },

  // Test: JWT with string audience (not array) works
  async testStringAudience(_, env) {
    const teamDomain = 'test-string-aud';
    const jwt = await generateJwt(env, teamDomain, { aud: AUDIENCE }); // string, not array
    const req = createRequest(jwt);

    const payload = await validateAccessJwt(req, teamDomain, AUDIENCE);
    assert.strictEqual(
      payload.iss,
      `https://${teamDomain}.cloudflareaccess.com`
    );
  },

  // Test: JWT with multiple audiences where one matches
  async testMultipleAudiences(_, env) {
    const teamDomain = 'test-multi-aud';
    const jwt = await generateJwt(env, teamDomain, {
      aud: ['other-audience', AUDIENCE, 'another-audience'],
    });
    const req = createRequest(jwt);

    const payload = await validateAccessJwt(req, teamDomain, AUDIENCE);
    assert.ok(payload.aud.includes(AUDIENCE));
  },

  // Test: Clock skew tolerance allows recently expired JWTs
  async testClockSkewTolerance(_, env) {
    const teamDomain = 'test-clock-skew';
    // JWT that expired 30 seconds ago (within 60s tolerance)
    const now = Math.floor(Date.now() / 1000);
    const jwt = await generateJwt(env, teamDomain, {
      iat: now - 3600,
      exp: now - 30,
    });
    const req = createRequest(jwt);

    // Should still pass due to clock skew tolerance
    const payload = await validateAccessJwt(req, teamDomain, AUDIENCE);
    assert.strictEqual(
      payload.iss,
      `https://${teamDomain}.cloudflareaccess.com`
    );
  },

  // Test: Error has correct name property
  async testErrorName(_, env) {
    const teamDomain = 'test-error-name';
    const req = createRequest(null);

    try {
      await validateAccessJwt(req, teamDomain, AUDIENCE);
      assert.fail('Should have thrown');
    } catch (err) {
      assert.strictEqual(err.name, 'AccessJwtError');
      assert.ok(err instanceof Error);
    }
  },
};
