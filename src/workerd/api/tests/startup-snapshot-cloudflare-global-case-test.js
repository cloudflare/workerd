// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A case of startup-snapshot-cloudflare-global-test.js: MODE says what the top level reads of
// `Cloudflare`.

import { env } from 'cloudflare:workers';
// Whether a Worker was started from a startup snapshot is asked at request time: at module scope
// the answer would be the zygote's and be baked into the snapshot.
import unsafe from 'workerd:unsafe';

const MODE = env.MODE;

function caught(fn) {
  try {
    fn();
  } catch (error) {
    return error;
  }
  return undefined;
}

// What the top level keeps. In 'kept': the object, an object inheriting from it, its flags, their
// names and one value, its constructor, and an error the constructor throws. In 'object': the
// object alone, its flags unread.
let kept = null;
if (MODE === 'kept') {
  const { compatibilityFlags } = globalThis.Cloudflare;
  kept = {
    Cloudflare: globalThis.Cloudflare,
    derived: Object.create(globalThis.Cloudflare),
    flags: compatibilityFlags,
    names: Reflect.ownKeys(compatibilityFlags).join(),
    nodejsCompat: compatibilityFlags.nodejs_compat,
    Constructor: globalThis.Cloudflare.constructor,
    error: caught(() => new globalThis.Cloudflare.constructor()),
  };
} else if (MODE === 'object') {
  kept = { Cloudflare: globalThis.Cloudflare };
}

// What holds of the object in every case.
function commonChecks() {
  const flags = Cloudflare.compatibilityFlags;
  const names = Reflect.ownKeys(flags);
  const Constructor = Cloudflare.constructor;
  return {
    sameObjectEveryTime: Cloudflare === globalThis.Cloudflare,
    ownKeys: Reflect.ownKeys(Cloudflare).join() === 'compatibilityFlags',
    isCloudflare:
      Cloudflare instanceof Constructor &&
      Object.getPrototypeOf(Cloudflare) === Constructor.prototype &&
      Constructor.name === 'Cloudflare',
    flagsAreTheSameObject: flags === Cloudflare.compatibilityFlags,
    flagsHaveNoPrototype: Object.getPrototypeOf(flags) === null,
    flagsAreFrozen: Object.isFrozen(flags),
    flagsAreBooleans:
      names.length > 100 &&
      names.every((name) => typeof flags[name] === 'boolean'),
    // The Worker's own flags: those its config names, and none it does not.
    flagsAreTheWorkers:
      flags.nodejs_compat === true &&
      flags.experimental === true &&
      flags.unsafe_module === true &&
      flags.enable_web_file_system === false,
    readOnly:
      caught(() => {
        'use strict';
        Cloudflare.compatibilityFlags = {};
      }) instanceof TypeError,
    notConstructible:
      caught(() => new Constructor())?.message === 'Illegal constructor',
    notSerializable:
      caught(() => structuredClone(Cloudflare))?.name === 'DataCloneError',
  };
}

function checks() {
  const common = commonChecks();
  if (kept === null) return common;
  if (MODE === 'object') {
    return { ...common, sameCloudflare: kept.Cloudflare === Cloudflare };
  }
  return {
    ...common,
    sameCloudflare: kept.Cloudflare === Cloudflare,
    sameFlags: kept.flags === Cloudflare.compatibilityFlags,
    sameNames:
      kept.names === Reflect.ownKeys(Cloudflare.compatibilityFlags).join(),
    keptValue: kept.nodejsCompat === true,
    keptFlagsAreFrozen:
      Object.isFrozen(kept.flags) && Object.getPrototypeOf(kept.flags) === null,
    sameConstructor: kept.Constructor === Cloudflare.constructor,
    derivedReads: kept.derived.compatibilityFlags === kept.flags,
    derivedIsOrdinary:
      Object.getPrototypeOf(structuredClone(kept.derived)) === Object.prototype,
    topLevelError:
      kept.error instanceof TypeError &&
      kept.error.message === 'Illegal constructor',
  };
}

export default {
  async fetch() {
    return Response.json({
      mode: MODE,
      snapshotUsed: unsafe.startedFromStartupSnapshot(),
      checks: checks(),
    });
  },
};
