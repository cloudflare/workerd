using Workerd = import "/workerd/workerd.capnp";

const multipleCodebasesExample :Workerd.Config = (
  services = [
    (name = "router", worker = (
      modules = [(name = "router", esModule = embed "router.js")],
      compatibilityDate = "2023-02-28",
      bindings = [
        (name = "APP_ONE", service = "app-one"),
        (name = "APP_TWO", service = "app-two"),
      ],
    )),
    (name = "app-one", worker = (
      modules = [(name = "app", esModule = embed "app-one.js")],
      compatibilityDate = "2023-02-28",
    )),
    (name = "app-two", worker = (
      modules = [(name = "app", esModule = embed "app-two.js")],
      compatibilityDate = "2023-02-28",
    )),
  ],
  sockets = [
    (name = "http", address = "127.0.0.1:8080", http = (), service = "router"),
  ],
);
