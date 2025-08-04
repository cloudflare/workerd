// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

export class DurableObjectExample extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.state = state;
  }

  async createTableForTestingAutoRollBackOnCriticalError() {
    this.state.storage.sql.exec(
      'CREATE TABLE IF NOT EXISTS test_full (id INTEGER PRIMARY KEY, data BLOB)'
    );
  }

  async createCriticalErrorThatLeadsToAutoRollback() {
    // Limit size of db so we can trigger a SQLITE_FULL error
    this.state.storage.sql.setMaxPageCountForTest(10);

    try {
      // Create large data to fill the database quickly
      const largeData = new Uint8Array(1000000).fill(42);
      this.state.storage.sql.exec(
        'INSERT INTO test_full VALUES (?, ?)',
        2,
        largeData
      );
      throw new Error(
        'should have thrown SQLITE_FULL exception before we reach here'
      );
    } catch (err) {
      if (err.message !== 'database or disk is full: SQLITE_FULL') {
        throw err;
      }
    }

    // With the OutputGate::onBroken() logging promise in place, adding an await here causes the
    // test to pass again, apparently because it allows IoContext::abort() to run, ending the
    // jsrpc call's execution here with the broken exception, and causing the next request for a
    // stub to go to a new instance.
    //
    //await scheduler.wait(0);

    const smallData = new Uint8Array(10).fill(1);
    // With the OutputGate::onBroken() logging promise in place, test fails because this exec
    // throws "Error: database or disk is full: SQLITE_FULL", but does not immediately break the
    // actor, so when the caller creates a new stub, it goes to the existing instance of the actor
    // instead of a new broken instance.
    this.state.storage.sql.exec(
      'INSERT INTO test_full VALUES (?, ?)',
      1,
      smallData
    );
  }

  async verifyAutoRollbackAfterCriticalError() {
    if (
      [...this.state.storage.sql.exec('SELECT * FROM test_full')].length != 0
    ) {
      throw new Error(
        'found data that was committed when a critical error happened'
      );
    }
  }
}

export let testAutoRollBackOnCriticalError = {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('auto-rollback-on-critical-error-test');
    let stub = env.ns.get(id);
    await stub.createTableForTestingAutoRollBackOnCriticalError();
    try {
      await stub.createCriticalErrorThatLeadsToAutoRollback();
      throw new Error(
        'should have thrown SQLITE_FULL exception before we reach here'
      );
    } catch (err) {
      if (!err.message.startsWith('database or disk is full: SQLITE_FULL')) {
        throw err;
      }
    }

    // With the OutputGate::onBroken() logging promise in place, adding an await here causes the
    // test to pass again, apparently because it allows IoContext::abort() to run, ensuring the
    // next request for a stub goes to a new instance.
    //
    //await scheduler.wait(0);

    // Get a new stub since the old stub is broken due to critical error
    stub = env.ns.get(id);
    await stub.verifyAutoRollbackAfterCriticalError();
  },
};
