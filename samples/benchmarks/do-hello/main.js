export class TestDO {
  constructor() { }

  async fetch() {
    return new Response("OK\n");
  }
}

export default {
  async fetch(request, env) {
    const id = env.testDO.idFromName("1");
    const testDO = env.testDO.get(id);
    return testDO.fetch("https://test/", request);
  }
}
