// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { DurableObject } from "cloudflare:workers";

export class MyDurableObject extends DurableObject<Env> {
	async setAlarm(): Promise<string> {
		await this.ctx.storage.setAlarm(Date.now() + 1000);
		return "Alarm set to fire in 1 second";
	}

	async alarm(): Promise<void> {
		console.log("Alarm triggered, about to throw an uncaught exception...\n");
		throw new Error("Intentional uncaught exception from DO alarm!\n");
	}
}

export default {
	async fetch(request, env, ctx): Promise<Response> {
		const url = new URL(request.url);

		if (url.pathname === "/set-alarm") {
			const stub = env.ns.idFromName("test-alarm");
			const obj = env.ns.get(stub);
			const result = await obj.setAlarm();
			return new Response(result);
		}

		return new Response("Use /set-alarm to trigger a DO alarm that throws an exception\n");
	},
} satisfies ExportedHandler<Env>;
