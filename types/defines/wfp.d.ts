// https://developers.cloudflare.com/cloudflare-for-platforms/workers-for-platforms/

interface DispatchNamespace {
	/**
	 * @param name Name of the Worker script.
	 * @returns A Fetcher object that allows you to send requests to the Worker script.
	 * @throws If the Worker script does not exist in this dispatch namespace, an error will be thrown.
	 */
	get(name: string): Fetcher;
}
