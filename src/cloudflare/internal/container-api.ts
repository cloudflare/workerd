// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import entrypoints from 'cloudflare-internal:workers';

export interface Container {
  get running(): boolean;
  start(options?: ContainerStartupOptions): void;
  monitor(): Promise<void>;
  destroy(error?: unknown): Promise<void>;
  signal(signo: number): void;
  getTcpPort(port: number): Fetcher;
  setInactivityTimeout(durationMs: number | bigint): Promise<void>;
}

interface ContainerStartupOptions {
  entrypoint?: string[];
  enableInternet: boolean;
  env?: Record<string, string>;
}

export interface ContainerClassStartupOptions {
  /** Environment variables to pass to the container */
  envVars?: ContainerStartupOptions['env'];
  /** Custom entrypoint to override container default */
  entrypoint?: ContainerStartupOptions['entrypoint'];
  /**
   * Whether to enable internet access for the container
   * @default true
   */
  enableInternet?: ContainerStartupOptions['enableInternet'];
  signal?: AbortSignal;
  /**
   * Whether to wait for the application inside the container to be ready
   * @default true
   */
  waitForReady?: boolean;

  retries?: {
    /**
     * Number of retries to check we have got a container
     * and if waitForReady is true, that it's ready
     * @default 10
     */
    limit?: number;
    /**
     * Timeout in milliseconds for each ping attempt
     * @default 5000
     */
    delay?: number;
    // TODO: make this accept exponential backoff
  };
  /** Port to check for readiness, defaults to `defaultPort` or 33 if not set */
  portToCheck?: number;
}

interface Fetcher {
  fetch: typeof fetch;
}

export interface DurableObjectContext {
  container?: Container;
  blockConcurrencyWhile<T>(callback: () => Promise<T>): Promise<T>;
  abort(): void;
}

export type State = {
  status: 'running' | 'stopped';
};

export type Signal = 'SIGKILL' | 'SIGINT' | 'SIGTERM';
export type SignalInteger = number;

const signalToNumbers: Record<Signal, SignalInteger> = {
  SIGINT: 2,
  SIGTERM: 15,
  SIGKILL: 9,
};

// ==== Defaults ====

const PING_TIMEOUT_MS = 5000;
const DEFAULT_SLEEP_AFTER = '10m';
const INSTANCE_POLL_INTERVAL_MS = 300;
const DEFAULT_RETRY_COUNT = 10;

// If user has specified no ports and we need to check one
// to see if the container is up at all.
const FALLBACK_PORT_TO_CHECK = 33;

// ==== Error helpers ====

const MAX_INSTANCES_ERROR =
  'Maximum number of running container instances exceeded';

const NO_CONTAINER_INSTANCE_ERROR =
  'there is no container instance that can be provided to this durable object';

const NOT_LISTENING_ERROR = 'the container is not listening';

function isErrorOfType(e: unknown, matchingString: string): boolean {
  const errorString = e instanceof Error ? e.message : String(e);
  return errorString.toLowerCase().includes(matchingString);
}

function startRetryableError(e: unknown): boolean {
  return (
    isErrorOfType(e, NO_CONTAINER_INSTANCE_ERROR) ||
    isErrorOfType(e, MAX_INSTANCES_ERROR)
  );
}

/**
 * Combines an user-defined signal with a timeout - we need a timeout
 * to make sure we don't hang forever waiting for a ping (defined by retries.delay)
 * but we also need to respect user aborts.
 * If the user didn't provide an abort signal, this is just the timeout.
 */
function addTimeoutSignals(
  userProvidedSignal: AbortSignal | undefined,
  timeoutMs: number
): AbortSignal {
  const controller = new AbortController();

  // Add timeout in case our pings hang
  const timeoutId = setTimeout(() => {
    controller.abort('ping timed out');
  }, timeoutMs);

  // If the user signal aborts, we want to cancel the timeout and clean up
  userProvidedSignal?.addEventListener('abort', () => {
    controller.abort();
    clearTimeout(timeoutId);
  });

  // note the timeout aborting does not clear the existing signal
  return controller.signal;
}

function parseTimeExpression(timeExpression: string | number): number {
  if (typeof timeExpression === 'number') {
    // If it's already a number, assume it's in seconds
    return timeExpression;
  }

  if (typeof timeExpression === 'string') {
    // Parse time expressions like "5m", "30s", "1h"
    const match = timeExpression.match(/^(\d+)([smh])$/);
    if (!match || !match[1]) {
      throw new Error(`invalid time expression ${timeExpression}`);
    }

    const value = parseInt(match[1]);
    const unit = match[2];

    // Convert to seconds based on unit
    switch (unit) {
      case 's':
        return value;
      case 'm':
        return value * 60;
      case 'h':
        return value * 60 * 60;
      default:
        throw new Error(`unknown time unit ${unit}`);
    }
  }

  throw new Error(
    `invalid type for a time expression: ${typeof timeExpression}`
  );
}

class ContainerClass<Env = unknown> extends entrypoints.DurableObject {
  defaultPort?: number;

  /**
   * Timeout for the container to sleep after inactivity.
   * Inactivity is defined as no requests to the container.
   * The signal sent to the container by default is a SIGTERM.
   */
  sleepAfter: string | number = DEFAULT_SLEEP_AFTER;

  // Container configuration properties
  // Set these properties directly in your container instance
  envVars: ContainerStartupOptions['env'];
  entrypoint: ContainerStartupOptions['entrypoint'];
  enableInternet: ContainerClassStartupOptions['enableInternet'] = true;
  retries: ContainerClassStartupOptions['retries'] = {
    limit: DEFAULT_RETRY_COUNT,
    delay: PING_TIMEOUT_MS,
  };

  container: Container;

  constructor(ctx: DurableObjectContext, env: Env) {
    super(ctx, env);

    if (ctx.container === undefined) {
      throw new Error(
        'Containers have not been enabled for this Durable Object class. Have you correctly setup your Wrangler config? More info: https://developers.cloudflare.com/containers/get-started/#configuration'
      );
    }

    // eslint-disable-next-line @typescript-eslint/no-floating-promises
    ctx.blockConcurrencyWhile(async () => {
      await ctx.container?.setInactivityTimeout(
        parseTimeExpression(this.sleepAfter) * 1000
      );
    });

    this.container = ctx.container;
  }

  /**
   * Gets the current state of the container
   */
  getState(): State {
    return {
      status: this.container.running
        ? ('running' as const)
        : ('stopped' as const),
    };
  }

  /**
   *
   * Starts container.
   * If the container is already started, and waitForReady is false, this is a no-op
   *
   */
  async start(options: ContainerClassStartupOptions = {}): Promise<void> {
    options.waitForReady ??= true;
    options.retries ??= {};
    options.retries.delay ??= this.retries?.delay ?? PING_TIMEOUT_MS;
    options.retries.limit ??= this.retries?.limit ?? DEFAULT_RETRY_COUNT;
    options.enableInternet ??= true;
    options.envVars ??= this.envVars;
    options.entrypoint ??= this.entrypoint;

    // If start was called via fetch, we will have a port passed in.
    // A user can also specify a port when calling start.
    // Oherwise we use the defaultPort property, and then a hardcoded fallback
    const portToCheck =
      options.portToCheck ?? this.defaultPort ?? FALLBACK_PORT_TO_CHECK;

    if (this.container.running) {
      return;
    }

    // we use a timeout so there is a bit of a cooldown between retries
    // but we still want to abort immediately if the user has requested it,
    // so we create this promise that we can race against our timeout.
    const userSignalPromise = new Promise<void>((res) => {
      options.signal?.addEventListener('abort', () => {
        res();
      });
    });

    // So we know if the last attempt to start resulted in a error where we
    // retry starting, or just retry the readiness check.
    let lastError: Error | undefined = undefined;
    let startupMonitor: Promise<void> | undefined;
    let attempt = 0;

    while (attempt < options.retries.limit) {
      if (options.signal?.aborted) {
        throw new Error('Container start aborted by user signal');
      }
      if (
         // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
        !this.container.running &&
        (attempt === 0 || startRetryableError(lastError))
      ) {
        const startOptions: ContainerStartupOptions = {
          enableInternet: options.enableInternet ?? this.enableInternet,
        };
        if (options.envVars && Object.keys(options.envVars).length > 0) {
          startOptions.env = options.envVars;
        }
        if (options.entrypoint && options.entrypoint.length > 0) {
          startOptions.entrypoint = options.entrypoint;
        }

        this.container.start(startOptions);
        lastError = undefined;
      }
      startupMonitor ??= this.container.monitor();

      try {
        // combine the user provided AbortSignal with a timeout
        const timeoutSignal = addTimeoutSignals(
          options.signal,
          options.retries.delay
        );
        await this.container
          .getTcpPort(portToCheck)
          .fetch('http://ping', { signal: timeoutSignal });

        // the ping was successful, exit the loop
        break;
      } catch (e) {
        // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
        if (this.container.running) {
          // exit the loop if the user has specified that we don't need to wait for the container application to be ready
          if (isErrorOfType(e, NOT_LISTENING_ERROR) && !options.waitForReady) {
            break;
          }
          // otherwise fallthrough to retry the ping...
        } else {
          // we started the container but it is now not running
          await startupMonitor.catch((err: unknown) => {
            // if the error is cloudchamberd not providing a container in time, we can retry starting
            if (startRetryableError(err) && err instanceof Error) {
              lastError = err;
            } else {
              // for any other reason, we should assume the container crashed and give up
              throw err;
            }
          });
          startupMonitor = undefined;
        }

        console.debug(
          'The container was not ready:',
          e instanceof Error ? e.message : String(e)
        );

        // we are out of retries
        if (attempt === options.retries.limit - 1) {
          if (
            e instanceof Error &&
            e.message.includes('Network connection lost')
          ) {
            // We have to abort here, the reasoning is that we might've found
            // ourselves in an internal error where the Worker is stuck with a failed connection to the
            // container services.
            //
            // Until we address this issue on the back-end CF side, we will need to abort the
            // durable object so it retries to reconnect from scratch.
            (this.ctx as DurableObjectContext).abort();
          }
          throw e;
        }
      }

      // Wait a bit before retrying
      await Promise.race([
        new Promise((res) => setTimeout(res, INSTANCE_POLL_INTERVAL_MS)),
        userSignalPromise,
      ]);
      attempt++;
    }
    // we have successfully exited the retry loop and the container is running

    await (this.ctx as DurableObjectContext).blockConcurrencyWhile(
      async () => {
        await this.onStart();
      }
    );

  }

  /**
   * Send a signal to the container.
   * @param signal - The signal to send to the container (default: 15 for SIGTERM)
   */
  stop(signal: Signal | SignalInteger = 'SIGTERM'): void {
    if (!this.container.running) {
      return;
    }
    this.container.signal(
      typeof signal === 'string' ? signalToNumbers[signal] : signal
    );
  }

  /**
   * Destroys the container with a SIGKILL.
   */
  async destroy(): Promise<void> {
    await this.container.destroy();
  }

  /**
   * Lifecycle method called when container starts successfully
   * Override this method in subclasses to handle container start events
   */
  onStart(): void | Promise<void> {}

  // this should not be overridden by the user
  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const targetPort =
      this.defaultPort ?? (url.port ? parseInt(url.port) : undefined);
    if (targetPort === undefined) {
      throw new Error(
        'No port configured for this container. Set the `defaultPort` in your Container subclass, or specify a port on your request url`.'
      );
    }

    await this.start({
      portToCheck: targetPort,
      waitForReady: true,
      signal: request.signal,
    });

    url.protocol = 'http:';
    return await this.container.getTcpPort(targetPort).fetch(url, request);
  }
}

export default { ContainerClass };
