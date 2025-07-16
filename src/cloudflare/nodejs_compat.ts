// Define the FetchEvent constructor
class FetchEvent extends Event {
  request: Request;
  clientId: string;

  constructor(type: string, init: { request: Request; clientId: string }) {
    super(type);
    this.request = init.request;
    this.clientId = init.clientId;
  }
}

export function registerFetchEvents() {
  return {
    async fetch(request: Request): Promise<Response> {
      const { promise, resolve } = Promise.withResolvers<Response>();
      const clientId = crypto.randomUUID();

      const event = new FetchEvent('nodejs.fetch', {
        request,
        clientId,
      });

      // Listen for the response event before dispatching
      globalThis.addEventListener(
        `nodejs.fetch-${clientId}`,
        ((event: CustomEvent<Response>) => {
          resolve(event.detail);
        }) as EventListener,
        { once: true }
      );

      globalThis.dispatchEvent(event);

      return promise;
    },
  };
}
