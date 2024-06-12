import { ok, strictEqual, throws } from 'node:assert';

const { compatibilityFlags } = globalThis.Cloudflare;
ok(Object.isSealed(compatibilityFlags));
ok(Object.isFrozen(compatibilityFlags));
ok(!Object.isExtensible(compatibilityFlags));

// It shuld be possible to shadow the Cloudflare global
const Cloudflare = 1;
strictEqual(Cloudflare, 1);

export const compatFlagsTest = {
  test() {
    // The compatibility flags object should be sealed, frozen, and not extensible.
    throws(() => (compatibilityFlags.no_nodejs_compat_v2 = '...'));
    throws(() => (compatibilityFlags.not_a_real_compat_flag = '...'));
    throws(() => {
      delete compatibilityFlags['nodejs_compat_v2'];
    });

    // The compatibility flags object should have no prototype.
    strictEqual(Object.getPrototypeOf(compatibilityFlags), null);

    // The compatibility flags object should have the expected properties...
    // That is... the only keys that should appear on the compatibilityFlags
    // object are the enable flags.
    //
    // If a key does not appear, it can mean one of three things:
    // 1. It is a disable flag.
    // 2. It is an experimental flag and the experimental option is not set.
    // 3. The flag does not exist.
    //
    // At this level we make no attempt to differentiate between these cases
    ok(compatibilityFlags['nodejs_compat_v2']);
    ok(compatibilityFlags['url_standard']);

    ok(!compatibilityFlags['no_nodejs_compat_v2']);
    ok(!compatibilityFlags['url_original']);
    strictEqual(compatibilityFlags['no_nodejs_compat_v2'], undefined);
    strictEqual(compatibilityFlags['url_original'], undefined);

    // Since we are not specifying the experimental flag, experimental flags should
    // not be included in the output.
    strictEqual(compatibilityFlags['durable_object_rename'], undefined);
    strictEqual('durable_object_rename' in compatibilityFlags, false);

    // If a flag does not exist, the value will be undefined.
    strictEqual(compatibilityFlags['not-a-real-compat-flag'], undefined);
    strictEqual('not-a-real-compat-flag' in compatibilityFlags, false);

    // The compatibility flags object should have the expected keys.
    const keys = Object.keys(compatibilityFlags);
    ok(keys.includes('nodejs_compat_v2'));
    ok(keys.includes('url_standard'));
    ok(!keys.includes('url_original'));
    ok(!keys.includes('not-a-real-compat-flag'));
  },
};
