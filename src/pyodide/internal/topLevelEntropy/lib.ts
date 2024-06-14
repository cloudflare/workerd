// @ts-nocheck

/**
 * Handle the top level getentropy() mess. See entropy_patches.py which is the
 * main file for the entropy patches.
 *
 * This file installs the relevant files and calls the exports from
 * entropy_patches.py. setupShouldAllowBadEntropy reads out the address of the
 * byte that we use to control calls to crypto.getRandomValues from Python.
 */
import { default as entropyPatches } from "pyodide-internal:topLevelEntropy/entropy_patches.py";
import { default as entropyImportContext } from "pyodide-internal:topLevelEntropy/entropy_import_context.py";
import { default as importPatchManager } from "pyodide-internal:topLevelEntropy/import_patch_manager.py";
import { IS_TRACING } from "pyodide-internal:metadata";
import { LOADED_SNAPSHOT_VERSION } from "pyodide-internal:snapshot";
import { simpleRunPython } from "pyodide-internal:util";

// TODO: When we've updated all the snapshots, remove this.
const SHOULD_GATE_ENTROPY =
  !IS_TRACING &&
  (LOADED_SNAPSHOT_VERSION === undefined || LOADED_SNAPSHOT_VERSION === 2);

let allowed_entropy_calls_addr;

/**
 * Set up a byte for communication between JS and Python.
 *
 * We make an array in Python and then get its address in JavaScript so
 * shouldAllowBadEntropy can check / write back the value
 */
function setupShouldAllowBadEntropy(Module) {
  // get_bad_entropy_flag prints the address we want into stderr which is returned into res.
  // We parse this as an integer.
  const res = simpleRunPython(
    Module,
    "from _cloudflare.entropy_import_context import get_bad_entropy_flag;" +
      "get_bad_entropy_flag();" +
      "del get_bad_entropy_flag",
  );
  allowed_entropy_calls_addr = Number(res);
}

function shouldAllowBadEntropy(Module) {
  if (!SHOULD_GATE_ENTROPY) {
    return true;
  }
  const val = Module.HEAP8[allowed_entropy_calls_addr];
  if (val) {
    Module.HEAP8[allowed_entropy_calls_addr]--;
    return true;
  }
  return false;
}

/**
 * Some packages need hash or random seeds at import time. We carefully track
 * how much bad entropy we're giving everyone so that hopefully none of it ends
 * up in a place where the end user needed good entropy. In particular, we think
 * it's acceptable to give poor entropy for hash seeds but not for random seeds.
 * The random libraries are allowed to initialize themselves with a bad seed but
 * we disable them until we have a chance to reseed.
 *
 * See entropy_import_context.py where `allow_bad_entropy_calls` is used to dole
 * out the bad entropy.
 */
export function getRandomValues(Module, arr) {
  try {
    return crypto.getRandomValues(arr);
  } catch (e) {
    if (
      !e.message.includes("Disallowed operation called within global scope")
    ) {
      Module._dump_traceback();
      throw e;
    }
    if (!shouldAllowBadEntropy(Module)) {
      Module._dump_traceback();
      throw e;
    }
    // "entropy" in the test suite is a bunch of 42's. Good to use a readily identifiable pattern
    // here which is different than the test suite.
    arr.fill(43);
  }
}

/**
 * We call this regardless of whether we are restoring from a snapshot or not,
 * after instantiating the Emscripten module but before restoring the snapshot.
 * Hypothetically, we could skip it for new dedicated snapshots.
 */
export function entropyMountFiles(Module) {
  Module.FS.mkdir(`/lib/python3.12/site-packages/_cloudflare`);
  Module.FS.writeFile(
    `/lib/python3.12/site-packages/_cloudflare/__init__.py`,
    new Uint8Array(0),
    { canOwn: true },
  );
  Module.FS.writeFile(
    `/lib/python3.12/site-packages/_cloudflare/entropy_patches.py`,
    new Uint8Array(entropyPatches),
    { canOwn: true },
  );
  Module.FS.writeFile(
    `/lib/python3.12/site-packages/_cloudflare/entropy_import_context.py`,
    new Uint8Array(entropyImportContext),
    { canOwn: true },
  );
  Module.FS.writeFile(
    `/lib/python3.12/site-packages/_cloudflare/import_patch_manager.py`,
    new Uint8Array(importPatchManager),
    { canOwn: true },
  );
}

/**
 * This prepares us to execute the top level scope. It changes JS state so it
 * needs to be called whether restoring snapshot or not. We have to call this
 * after the runtime is ready, so after restoring the snapshot in the snapshot
 * branch and after entropyMountFiles in the no-snapshot branch.
 */
export function entropyAfterRuntimeInit(Module) {
  setupShouldAllowBadEntropy(Module);
}

/**
 * This prepares us to execute the top level scope. It changes only Python state
 * so it doesn't need to be called when restoring from snapshot.
 */
export function entropyBeforeTopLevel(Module) {
  if (!SHOULD_GATE_ENTROPY) {
    return;
  }
  simpleRunPython(
    Module,
    `
from _cloudflare.entropy_patches import before_top_level
before_top_level()
del before_top_level
`,
  );
}

let isReady = false;
/**
 * Called to reseed rngs and turn off blocks that prevent access to rng APIs.
 */
export function entropyBeforeRequest(Module) {
  if (isReady) {
    // I think this is only ever called once, but we guard it just to be sure.
    return;
  }
  isReady = true;
  if (SHOULD_GATE_ENTROPY) {
    simpleRunPython(
      Module,
      `
from _cloudflare.entropy_patches import before_first_request
before_first_request()
del before_first_request
    `,
    );
  } else {
    // If we shouldn't gate entropy, we just need to reseed_rng. We first have
    // to call invalidate_caches b/c the snapshot doesn't know about
    // _cloudflare.entropy_patches.
    simpleRunPython(
      Module,
      `
from importlib import invalidate_caches
invalidate_caches()
del invalidate_caches
from _cloudflare.entropy_patches import reseed_rng
reseed_rng()
del reseed_rng
    `,
    );
  }
}
