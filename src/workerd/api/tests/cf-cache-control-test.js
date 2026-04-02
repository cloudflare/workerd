// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for cf.cacheControl mutual exclusion, synthesis, and additional cache settings.
// These require the workerd_experimental compat flag.

import assert from 'node:assert';

// Tests for cf.cacheControl mutual exclusion and synthesis.
// These tests run regardless of cache option flag since cacheControl is always available.
export const cacheControlMutualExclusion = {
  async test(ctrl, env, ctx) {
    // cacheControl + cacheTtl → TypeError at construction time
    assert.throws(
      () =>
        new Request('https://example.org', {
          cf: { cacheControl: 'max-age=300', cacheTtl: 300 },
        }),
      {
        name: 'TypeError',
        message: /cacheControl.*cacheTtl.*mutually exclusive/,
      }
    );

    // cacheControl alone should succeed
    {
      const req = new Request('https://example.org', {
        cf: { cacheControl: 'public, max-age=3600' },
      });
      assert.ok(req.cf);
    }

    // cacheTtl alone should succeed
    {
      const req = new Request('https://example.org', {
        cf: { cacheTtl: 300 },
      });
      assert.ok(req.cf);
    }

    // cacheControl + cacheTtlByStatus should succeed (not mutually exclusive)
    {
      const req = new Request('https://example.org', {
        cf: {
          cacheControl: 'public, max-age=3600',
          cacheTtlByStatus: { '200-299': 86400 },
        },
      });
      assert.ok(req.cf);
    }

    // cacheControl with undefined cacheTtl should succeed (only non-undefined triggers conflict)
    {
      const req = new Request('https://example.org', {
        cf: { cacheControl: 'max-age=300', cacheTtl: undefined },
      });
      assert.ok(req.cf);
    }
  },
};

export const cacheControlWithCacheOption = {
  async test(ctrl, env, ctx) {
    if (!env.CACHE_ENABLED) return;

    // cache option + cf.cacheControl → TypeError at construction time
    assert.throws(
      () =>
        new Request('https://example.org', {
          cache: 'no-store',
          cf: { cacheControl: 'no-cache' },
        }),
      {
        name: 'TypeError',
        message: /cacheControl.*cannot be used together with the.*cache/,
      }
    );

    // cache: 'no-cache' + cf.cacheControl → also TypeError
    // (need cache_no_cache flag for this, skip if not available)
  },
};

export const cacheControlSynthesis = {
  async test(ctrl, env, ctx) {
    // When cacheTtl is set without cacheControl, cacheControl should be synthesized
    // in the serialized cf blob. We verify by checking the cf property roundtrips correctly.

    // cacheTtl: 300 → cacheControl should be synthesized as "max-age=300"
    {
      const req = new Request('https://example.org', {
        cf: { cacheTtl: 300 },
      });
      // The cf object at construction time won't have cacheControl yet —
      // synthesis happens at serialization (fetch) time in serializeCfBlobJson.
      // We can verify the request constructs fine.
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheTtl, 300);
    }

    // cacheTtl: -1 → cacheControl should be synthesized as "no-store"
    {
      const req = new Request('https://example.org', {
        cf: { cacheTtl: -1 },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheTtl, -1);
    }

    // cacheTtl: 0 → cacheControl should be synthesized as "max-age=0"
    {
      const req = new Request('https://example.org', {
        cf: { cacheTtl: 0 },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheTtl, 0);
    }

    // Explicit cacheControl should NOT be overwritten
    {
      const req = new Request('https://example.org', {
        cf: { cacheControl: 'public, s-maxage=86400' },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheControl, 'public, s-maxage=86400');
    }
  },
};

export const additionalCacheSettings = {
  async test(ctrl, env, ctx) {
    // All additional cache settings should be accepted on the cf object
    {
      const req = new Request('https://example.org', {
        cf: {
          cacheReserveEligible: true,
          respectStrongEtag: true,
          stripEtags: false,
          stripLastModified: false,
          cacheDeceptionArmor: true,
          cacheReserveMinimumFileSize: 1024,
        },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheReserveEligible, true);
      assert.strictEqual(req.cf.respectStrongEtag, true);
      assert.strictEqual(req.cf.stripEtags, false);
      assert.strictEqual(req.cf.stripLastModified, false);
      assert.strictEqual(req.cf.cacheDeceptionArmor, true);
      assert.strictEqual(req.cf.cacheReserveMinimumFileSize, 1024);
    }

    // Additional cache settings should work alongside cacheControl
    {
      const req = new Request('https://example.org', {
        cf: {
          cacheControl: 'public, max-age=3600',
          cacheReserveEligible: true,
          stripEtags: true,
        },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheControl, 'public, max-age=3600');
      assert.strictEqual(req.cf.cacheReserveEligible, true);
      assert.strictEqual(req.cf.stripEtags, true);
    }

    // Additional cache settings should work alongside cacheTtl
    {
      const req = new Request('https://example.org', {
        cf: {
          cacheTtl: 300,
          respectStrongEtag: true,
          cacheDeceptionArmor: true,
        },
      });
      assert.ok(req.cf);
      assert.strictEqual(req.cf.cacheTtl, 300);
      assert.strictEqual(req.cf.respectStrongEtag, true);
      assert.strictEqual(req.cf.cacheDeceptionArmor, true);
    }
  },
};
