export default {
	async fetch(request) {
		const url = new URL(request.url);
		const headers = new Headers(request.headers);
		const userAgent = headers.get('User-Agent');
		headers.set('Test', 'good');
		await request.arrayBuffer();
		return new Response(null);
	}
};
