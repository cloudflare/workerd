// test.ts
function notAvailable(name) {
  throw new Error(
    `bun:test ${name}() is not available in workerd. Run tests with 'bun test' instead.`
  );
}
function describe(_name, _fn) {
  notAvailable("describe");
}
function it(_name, _fn, _options) {
  notAvailable("it");
}
function test(_name, _fn, _options) {
  notAvailable("test");
}
function expect(_value) {
  notAvailable("expect");
}
function beforeAll(_fn) {
  notAvailable("beforeAll");
}
function afterAll(_fn) {
  notAvailable("afterAll");
}
function beforeEach(_fn) {
  notAvailable("beforeEach");
}
function afterEach(_fn) {
  notAvailable("afterEach");
}
function mock(_fn) {
  notAvailable("mock");
}
function spyOn(_object, _method) {
  notAvailable("spyOn");
}
function setSystemTime(_time) {
  notAvailable("setSystemTime");
}
var test_default = {
  describe,
  it,
  test,
  expect,
  beforeAll,
  afterAll,
  beforeEach,
  afterEach,
  mock,
  spyOn,
  setSystemTime
};
export {
  afterAll,
  afterEach,
  beforeAll,
  beforeEach,
  test_default as default,
  describe,
  expect,
  it,
  mock,
  setSystemTime,
  spyOn,
  test
};
