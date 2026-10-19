// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import assert from 'node:assert';

export const unknownGenerationFieldStaysIgnored = {
  async test(ctrl, env) {
    assert.equal(env.loader.createGeneration, undefined);
    const code = {
      compatibilityDate: '2026-10-01',
      mainModule: 'main.js',
      globalOutbound: null,
      modules: {
        'main.js': `
        export default { fetch() { return new Response('legacy'); } };
      `,
      },
    };
    Object.defineProperty(code, 'generation', {
      configurable: true,
      get() {
        throw new Error('disabled generation getter must stay unread');
      },
    });
    const loaded = env.loader.load(code).getEntrypoint();
    assert.equal(
      await (await loaded.fetch('https://example.com')).text(),
      'legacy'
    );
    Object.defineProperty(code, 'generation', { value: 123 });
    const fetched = env.loader.get('legacy', () => code).getEntrypoint();
    assert.equal(
      await (await fetched.fetch('https://example.com')).text(),
      'legacy'
    );
  },
};
