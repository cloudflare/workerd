// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A case of startup-snapshot-navigator-test.js: MODE says whether the top level reads
// `navigator`.

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

// What the top level keeps: the navigator, an object inheriting from it, its constructor and
// method, the objects two of its attributes are, what sendBeacon() returns outside a request,
// and an error the constructor throws.
const kept =
  MODE === 'kept'
    ? {
        navigator,
        derived: Object.create(navigator),
        Navigator,
        sendBeacon: navigator.sendBeacon,
        languages: navigator.languages,
        storage: navigator.storage,
        keys: Reflect.ownKeys(navigator).join(),
        beacon: navigator.sendBeacon('http://sink/top-level', 'body'),
        error: caught(() => new Navigator()),
      }
    : null;

const kKeys =
  'storage,languages,language,platform,hardwareConcurrency,userAgent';

// What holds of the navigator in either case.
async function navigatorChecks() {
  const languages = navigator.languages;
  const root = await navigator.storage.getDirectory();
  return {
    sameObjectEveryTime: navigator === navigator,
    isNavigator:
      navigator instanceof Navigator &&
      Object.getPrototypeOf(navigator) === Navigator.prototype,
    keys: Reflect.ownKeys(navigator).join() === kKeys,
    userAgent: navigator.userAgent === 'Cloudflare-Workers',
    hardwareConcurrency: navigator.hardwareConcurrency === 1,
    platform: navigator.platform === '',
    language: navigator.language === 'en',
    languages: languages.length === 1 && languages[0] === 'en',
    languagesIsANewArrayEachTime: languages !== navigator.languages,
    languagesIsOfThisRealm:
      Object.getPrototypeOf(languages) === Array.prototype,
    constructorIsOfThisRealm:
      Object.getPrototypeOf(Navigator) === Function.prototype,
    errorIsOfThisRealm: caught(() => new Navigator()) instanceof TypeError,
    storageIsTheSameObject: navigator.storage === navigator.storage,
    storageWorks: root instanceof FileSystemDirectoryHandle,
    sendsBeacon: navigator.sendBeacon('http://sink/request', 'body') === true,
    checksReceiver:
      caught(() => navigator.sendBeacon.call({}, 'http://sink/')) instanceof
      TypeError,
    notConstructible:
      caught(() => new Navigator())?.message === 'Illegal constructor',
    readOnly:
      caught(() => {
        'use strict';
        navigator.userAgent = 'changed';
      }) instanceof TypeError,
    notSerializable:
      caught(() => structuredClone(navigator))?.name === 'DataCloneError',
  };
}

async function checks() {
  const common = await navigatorChecks();
  if (kept === null) return common;
  return {
    ...common,
    sameNavigator: kept.navigator === navigator,
    sameConstructor: kept.Navigator === Navigator,
    sameMethod: kept.sendBeacon === navigator.sendBeacon,
    sameKeys: kept.keys === kKeys,
    sameStorage: kept.storage === navigator.storage,
    keptLanguages:
      kept.languages.length === 1 &&
      kept.languages[0] === 'en' &&
      kept.languages !== navigator.languages,
    derivedReads:
      kept.derived.userAgent === 'Cloudflare-Workers' &&
      kept.derived.languages[0] === 'en' &&
      kept.derived.storage === kept.storage,
    derivedIsNoNavigator:
      caught(() => kept.derived.sendBeacon('http://sink/')) instanceof
      TypeError,
    keptMethodSendsBeacon:
      kept.sendBeacon.call(kept.navigator, 'http://sink/kept') === true,
    noBeaconAtTopLevel: kept.beacon === false,
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
      checks: await checks(),
    });
  },
};
