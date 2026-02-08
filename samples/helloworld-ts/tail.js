export default {
	async tail(events) {
		for (const event of events) {
			console.log(JSON.stringify(event, null, 2));
		}
	},
};