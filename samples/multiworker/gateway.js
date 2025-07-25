import { WorkerEntrypoint } from "cloudflare:workers"
import { rpcOverWebSocket } from "rpc.js"

async function proxyFetch(request, env) {
	const proxiedHeaders = new Headers();
	for (const [name, value] of request.headers) {
		// The `Upgrade` header needs to be special-cased to prevent:
		//   TypeError: Worker tried to return a WebSocket in a response to a request which did not contain the header "Upgrade: websocket"
		if (name === "upgrade" || name.startsWith("MF-")) {
			proxiedHeaders.set(name, value);
		} else {
			proxiedHeaders.set(`MF-Header-${name}`, value);
		}
	}
	proxiedHeaders.set("MF-URL", request.url);
	proxiedHeaders.set("MF-Binding", env.binding);
	const req = new Request(request, {
		headers: proxiedHeaders,
	});

	return fetch(env.remoteProxyConnectionString, req);
}

export class Gateway extends WorkerEntrypoint {
	async fetch(request) {
		return proxyFetch(request, this.env);
	}

	constructor(ctx, env) {
    console.log("constructor")
		const url = new URL("http://localhost:8081");
		url.protocol = "ws:";
		const session = rpcOverWebSocket(url.href);
		const stub = session.getStub();

		super(ctx, env);
    this.ctx.waitUntil(scheduler.wait(5_000))

		return new Proxy(this, {
			get(target, prop) {
				console.log(stub, prop);

				if (Reflect.has(target, prop)) {
					return Reflect.get(target, prop);
				}

				const value =  Reflect.get(stub, prop);
        console.log("PROXY", value)
        return value
			},
		});
	}
}

export default {
  fetch() {}
}
