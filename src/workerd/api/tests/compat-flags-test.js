import {
  ok,
  throws,
} from 'node:assert';

import { compatibilityFlags } from 'cloudflare:workers';

export const compatFlagsTest = {
  test() {
    throws(() => compatibilityFlags.no_nodejs_compat_v2 = true);
    throws(() => compatibilityFlags.not_a_real_compat_flag = true);
    ok(compatibilityFlags['nodejs_compat_v2']);
    ok(!compatibilityFlags['no_nodejs_compat_v2']);
    ok(compatibilityFlags['url_standard']);
    ok(!compatibilityFlags['url_original']);
    ok(!compatibilityFlags['not-a-real-compat-flag']);
    const keys = Object.keys(compatibilityFlags);
    ok(keys.includes('nodejs_compat_v2'));
    ok(keys.includes('url_standard'));
    ok(keys.includes('url_original'));
    ok(!keys.includes('not-a-real-compat-flag'));
  }
}
