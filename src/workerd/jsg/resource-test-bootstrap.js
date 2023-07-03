class BootstrapClassImpl {
  foo() { return "foo"; }
}

export function bootstrap(global) {
  if (!global) throw new Error("global context object not specified");

  global.BootstrapClass = BootstrapClassImpl;
}
