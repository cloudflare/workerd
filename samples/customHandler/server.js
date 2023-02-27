export default {
  async fetchJson(json) {
    return { type: "pong", request: json };
  }
};
