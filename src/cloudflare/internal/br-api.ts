// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import wrappedBinding from 'cloudflare-internal:wrapped-binding';

// Pairs each `BrowserRun.quickAction()` overload's action with its options.
type QuickActionOptions = {
  screenshot: BrowserRunScreenshotOptions;
  pdf: BrowserRunPDFOptions;
  content: BrowserRunContentOptions;
  scrape: BrowserRunScrapeOptions;
  links: BrowserRunLinksOptions;
  snapshot: BrowserRunSnapshotOptions;
  json: BrowserRunJsonOptions;
  markdown: BrowserRunMarkdownOptions;
  accessibilityTree: BrowserRunAccessibilityTreeOptions;
};

interface Fetcher {
  fetch: typeof fetch;
  quickAction<Action extends keyof QuickActionOptions>(
    action: Action,
    options: QuickActionOptions[Action]
  ): Promise<Response>;
  acquire(options?: BrowserRunAcquireOptions): Promise<BrowserRunAcquireResult>;
  launch(options?: BrowserRunAcquireOptions): Promise<BrowserRunConnection>;
  connectSession(
    sessionId: string,
    options?: BrowserRunConnectOptions
  ): Promise<BrowserRunConnection>;
  getLiveView(
    sessionId: string,
    options?: BrowserRunLiveViewOptions
  ): Promise<BrowserRunLiveView>;
  listSessions(
    options?: BrowserRunListSessionsOptions
  ): Promise<BrowserRunSession[]>;
  history(options?: BrowserRunHistoryOptions): Promise<BrowserRunSession[]>;
  limits(): Promise<BrowserRunLimits>;
  getSession(sessionId: string): Promise<BrowserRunSession | null>;
  closeSession(sessionId: string): Promise<BrowserRunCloseSessionResult>;
  readonly devtools: BrowserRunDevtools;
}

type WebSocketOption = { websocket?: boolean };

type LaunchOptions = BrowserRunAcquireOptions & WebSocketOption;

type ConnectSessionOptions = BrowserRunConnectOptions & WebSocketOption;

type WebSocketConnection = Omit<BrowserRunConnection, 'webSocket'> & {
  webSocket: WebSocket;
};

type UpgradeResponse = Response & { webSocket: WebSocket | null };

export class BrowserRendering extends wrappedBinding.WrappedBinding {
  readonly #fetcher: Fetcher;

  constructor(fetcher: Fetcher) {
    super(fetcher);
    this.#fetcher = fetcher;
  }

  async fetch(input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
    return this.#fetcher.fetch(input, init);
  }

  quickAction<Action extends keyof QuickActionOptions>(
    action: Action,
    options: QuickActionOptions[Action]
  ): Promise<Response> {
    return this.#fetcher.quickAction(action, options);
  }

  acquire(
    options?: BrowserRunAcquireOptions
  ): Promise<BrowserRunAcquireResult> {
    return this.#fetcher.acquire(options);
  }

  launch({ websocket, ...options }: LaunchOptions = {}): Promise<
    BrowserRunConnection | WebSocketConnection
  > {
    const connection = this.#fetcher.launch(options);
    if (!websocket) {
      return connection;
    }
    return connection.then(async (con) => {
      try {
        return await upgrade(con);
      } catch (err) {
        await this.#fetcher.closeSession(con.sessionId).catch(() => undefined);
        throw err;
      }
    });
  }

  connectSession(
    sessionId: string,
    { websocket, ...options }: ConnectSessionOptions = {}
  ): Promise<BrowserRunConnection | WebSocketConnection> {
    const connection = this.#fetcher.connectSession(sessionId, options);
    return websocket ? connection.then(upgrade) : connection;
  }

  getLiveView(
    sessionId: string,
    options?: BrowserRunLiveViewOptions
  ): Promise<BrowserRunLiveView> {
    return this.#fetcher.getLiveView(sessionId, options);
  }

  listSessions(
    options?: BrowserRunListSessionsOptions
  ): Promise<BrowserRunSession[]> {
    return this.#fetcher.listSessions(options);
  }

  history(options?: BrowserRunHistoryOptions): Promise<BrowserRunSession[]> {
    return this.#fetcher.history(options);
  }

  limits(): Promise<BrowserRunLimits> {
    return this.#fetcher.limits();
  }

  getSession(sessionId: string): Promise<BrowserRunSession | null> {
    return this.#fetcher.getSession(sessionId);
  }

  closeSession(sessionId: string): Promise<BrowserRunCloseSessionResult> {
    return this.#fetcher.closeSession(sessionId);
  }

  get devtools(): BrowserRunDevtools {
    return this.#fetcher.devtools;
  }
}

async function upgrade(
  connection: BrowserRunConnection
): Promise<WebSocketConnection> {
  const response = await connection.webSocket.fetch(
    'https://browser-binding.invalid/',
    {
      headers: { Upgrade: 'websocket' },
    }
  );
  const webSocket = (response as UpgradeResponse).webSocket;
  if (response.status !== 101 || !webSocket) {
    const body = await response.text();
    throw new Error(
      `Browser Run WebSocket upgrade failed with HTTP ${response.status}: ${body}`
    );
  }
  return Object.assign(connection, { webSocket });
}

export default function makeBinding(env: {
  fetcher: Fetcher;
}): BrowserRendering {
  return new BrowserRendering(env.fetcher);
}
