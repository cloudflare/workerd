import cluster from 'node:cluster';
import assert from 'node:assert';

export const clusterConstants = {
  test() {
    // Test scheduling constants
    assert.strictEqual(cluster.SCHED_NONE, 1);
    assert.strictEqual(cluster.SCHED_RR, 2);

    // Test the default scheduling policy
    assert.strictEqual(cluster.schedulingPolicy, cluster.SCHED_RR);
  },
};

export const clusterMasterWorkerFlags = {
  test() {
    // Test master/primary flags
    assert.strictEqual(cluster.isMaster, true);
    assert.strictEqual(cluster.isPrimary, true);
    assert.strictEqual(cluster.isWorker, false);

    // Test that isMaster and isPrimary are the same
    assert.strictEqual(cluster.isMaster, cluster.isPrimary);
  },
};

export const clusterSettings = {
  test() {
    // Settings should be an empty object
    assert.deepStrictEqual(cluster.settings, {});
    assert.strictEqual(typeof cluster.settings, 'object');
  },
};

export const clusterWorkers = {
  test() {
    // Workers should be an empty object
    assert.deepStrictEqual(cluster.workers, {});
    assert.strictEqual(typeof cluster.workers, 'object');
  },
};

export const clusterForkMethod = {
  test() {
    // Test fork method throws ERR_METHOD_NOT_IMPLEMENTED
    assert.throws(() => cluster.fork(), { code: 'ERR_METHOD_NOT_IMPLEMENTED' });

    // Test fork with env parameter
    assert.throws(() => cluster.fork({ NODE_ENV: 'test' }), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};

export const clusterDisconnectMethod = {
  test() {
    // Test disconnect method throws ERR_METHOD_NOT_IMPLEMENTED
    assert.throws(() => cluster.disconnect(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};

export const clusterSetupMethods = {
  test() {
    // Test setupPrimary method throws ERR_METHOD_NOT_IMPLEMENTED
    assert.throws(() => cluster.setupPrimary(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });

    // Test setupMaster method throws ERR_METHOD_NOT_IMPLEMENTED
    assert.throws(() => cluster.setupMaster(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};

export const clusterWorkerClass = {
  test() {
    // Create a Worker instance
    const Worker = cluster.Worker;
    const worker = new Worker();

    // Test initial properties
    assert.strictEqual(worker._connected, false);
    assert.strictEqual(worker.id, 0);
    assert.strictEqual(worker.exitedAfterDisconnect, false);

    // Test isConnected method
    assert.strictEqual(worker.isConnected(), false);

    // Test isDead method
    assert.strictEqual(worker.isDead(), true);

    // Test send method always returns false
    assert.strictEqual(worker.send('message'), false);
    assert.strictEqual(worker.send('message', null), false);
    assert.strictEqual(worker.send('message', null, {}), false);
    assert.strictEqual(
      worker.send('message', null, {}, () => {}),
      false
    );

    // Test kill method
    worker._connected = true;
    worker.kill();
    assert.strictEqual(worker._connected, false);

    // Test kill with signal
    worker._connected = true;
    worker.kill('SIGTERM');
    assert.strictEqual(worker._connected, false);

    // Test destroy method
    worker._connected = true;
    worker.destroy();
    assert.strictEqual(worker._connected, false);

    // Test destroy with signal
    worker._connected = true;
    worker.destroy('SIGTERM');
    assert.strictEqual(worker._connected, false);

    // Test disconnect method returns this
    worker._connected = true;
    const result = worker.disconnect();
    assert.strictEqual(result, worker);
    assert.strictEqual(worker._connected, false);

    // Test process property
    assert.strictEqual(worker.process, globalThis.process);

    // Test Worker extends EventEmitter
    assert.strictEqual(typeof worker.on, 'function');
    assert.strictEqual(typeof worker.emit, 'function');
    assert.strictEqual(typeof worker.removeListener, 'function');
  },
};

export const clusterEventEmitter = {
  test() {
    // Test that cluster extends EventEmitter
    assert.strictEqual(typeof cluster.on, 'function');
    assert.strictEqual(typeof cluster.emit, 'function');
    assert.strictEqual(typeof cluster.once, 'function');
    assert.strictEqual(typeof cluster.removeListener, 'function');
    assert.strictEqual(typeof cluster.removeAllListeners, 'function');
    assert.strictEqual(typeof cluster.setMaxListeners, 'function');
    assert.strictEqual(typeof cluster.getMaxListeners, 'function');
    assert.strictEqual(typeof cluster.listeners, 'function');
    assert.strictEqual(typeof cluster.rawListeners, 'function');
    assert.strictEqual(typeof cluster.listenerCount, 'function');
    assert.strictEqual(typeof cluster.prependListener, 'function');
    assert.strictEqual(typeof cluster.prependOnceListener, 'function');
    assert.strictEqual(typeof cluster.eventNames, 'function');

    // Test adding and removing listeners
    let called = false;
    const listener = () => {
      called = true;
    };

    cluster.on('test', listener);
    cluster.emit('test');
    assert.strictEqual(called, true);

    called = false;
    cluster.removeListener('test', listener);
    cluster.emit('test');
    assert.strictEqual(called, false);
  },
};

export const clusterInstanceProperties = {
  test() {
    // Test that cluster instance has all expected properties
    assert.strictEqual(cluster.isMaster, true);
    assert.strictEqual(cluster.isPrimary, true);
    assert.strictEqual(cluster.isWorker, false);
    assert.strictEqual(cluster.SCHED_NONE, 1);
    assert.strictEqual(cluster.SCHED_RR, 2);
    assert.strictEqual(cluster.schedulingPolicy, 2);
    assert.deepStrictEqual(cluster.settings, {});
    assert.deepStrictEqual(cluster.workers, {});
    assert.strictEqual(typeof cluster.Worker, 'function');
    assert.strictEqual(typeof cluster.setupPrimary, 'function');
    assert.strictEqual(typeof cluster.setupMaster, 'function');
    assert.strictEqual(typeof cluster.disconnect, 'function');
    assert.strictEqual(typeof cluster.fork, 'function');
  },
};

export const clusterModuleExports = {
  async test() {
    // Test module exports
    const clusterModule = await import('node:cluster');

    // Test default export
    assert.strictEqual(typeof clusterModule.default, 'object');
    assert.strictEqual(clusterModule.default.isPrimary, true);

    // Test named exports
    assert.strictEqual(clusterModule.SCHED_NONE, 1);
    assert.strictEqual(clusterModule.SCHED_RR, 2);
    assert.strictEqual(clusterModule.isMaster, true);
    assert.strictEqual(clusterModule.isPrimary, true);
    assert.strictEqual(clusterModule.isWorker, false);
    assert.strictEqual(clusterModule.schedulingPolicy, 2);
    assert.deepStrictEqual(clusterModule.settings, {});
    assert.deepStrictEqual(clusterModule.workers, {});
    assert.strictEqual(typeof clusterModule.Worker, 'function');
    assert.strictEqual(typeof clusterModule.Cluster, 'function');
    assert.strictEqual(typeof clusterModule.fork, 'function');
    assert.strictEqual(typeof clusterModule.disconnect, 'function');
    assert.strictEqual(typeof clusterModule.setupPrimary, 'function');
    assert.strictEqual(typeof clusterModule.setupMaster, 'function');
  },
};

export const clusterWorkerEventEmitter = {
  test() {
    const Worker = cluster.Worker;
    const worker = new Worker();

    // Test Worker event emitter functionality
    let eventFired = false;
    const handler = () => {
      eventFired = true;
    };

    worker.on('custom', handler);
    worker.emit('custom');
    assert.strictEqual(eventFired, true);

    eventFired = false;
    worker.removeListener('custom', handler);
    worker.emit('custom');
    assert.strictEqual(eventFired, false);

    // Test once
    let onceCount = 0;
    worker.once('once-event', () => {
      onceCount++;
    });
    worker.emit('once-event');
    worker.emit('once-event');
    assert.strictEqual(onceCount, 1);
  },
};

export const clusterClassInstance = {
  test() {
    // Test creating a new Cluster instance
    const ClusterClass = cluster.constructor;
    const newCluster = new ClusterClass();

    // Test that the new instance has all expected properties
    assert.strictEqual(newCluster.isMaster, true);
    assert.strictEqual(newCluster.isPrimary, true);
    assert.strictEqual(newCluster.isWorker, false);
    assert.strictEqual(newCluster.SCHED_NONE, 1);
    assert.strictEqual(newCluster.SCHED_RR, 2);
    assert.strictEqual(newCluster.schedulingPolicy, 2);
    assert.deepStrictEqual(newCluster.settings, {});
    assert.deepStrictEqual(newCluster.workers, {});
    assert.strictEqual(typeof newCluster.Worker, 'function');

    // Test that methods throw the expected errors
    assert.throws(() => newCluster.setupPrimary(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });

    assert.throws(() => newCluster.setupMaster(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });

    assert.throws(() => newCluster.disconnect(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });

    assert.throws(() => newCluster.fork(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
    });
  },
};
