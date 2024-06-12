import {
  ok,
  throws,
} from 'node:assert';

import { compatFlags } from 'cloudflare:workers';

export const compatFlagsTest = {
  test() {
    throws(() => compatFlags.no_nodejs_compat_v2 = true);
    throws(() => compatFlags.not_a_real_compat_flag = true);
    ok(compatFlags['nodejs_compat_v2']);
    ok(!compatFlags['no_nodejs_compat_v2']);
    ok(compatFlags['url_standard']);
    ok(!compatFlags['url_original']);
    ok(!compatFlags['not-a-real-compat-flag']);
    const keys = Object.keys(compatFlags);
    ok(keys.includes('nodejs_compat_v2'));
    ok(keys.includes('url_standard'));
    ok(keys.includes('url_original'));
    ok(!keys.includes('not-a-real-compat-flag'));
  }
}
