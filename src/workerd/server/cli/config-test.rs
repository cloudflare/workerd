use super::*;

fn read(config: &Config) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    capnp::serialize::read_message(bytes_from_words(&config.0).as_slice(), reader_options())
        .unwrap()
}

#[test]
fn python_baseline_config_has_one_python_worker() {
    use workerd_capnp::service;
    use workerd_capnp::worker;

    let message = read(&Config::python_baseline("python_workers_20250116"));
    let config = message.get_root::<config::Reader>().unwrap();
    let services = config.get_services().unwrap();
    assert_eq!(services.len(), 1);
    let service = services.get(0);
    assert_eq!(service.get_name().unwrap(), "main");
    let service::Which::Worker(worker) = service.which().unwrap() else {
        panic!("expected a worker service")
    };
    let worker = worker.unwrap();
    let flags = worker.get_compatibility_flags().unwrap();
    assert_eq!(flags.get(0).unwrap(), "python_workers_20250116");
    assert_eq!(flags.get(1).unwrap(), "python_workers");
    let worker::Which::Modules(modules) = worker.which().unwrap() else {
        panic!("expected a modules worker")
    };
    assert_eq!(modules.unwrap().get(0).get_name().unwrap(), "main.py");
}

#[test]
fn config_bytes_are_checked() {
    let good = Config::python_baseline("python_workers");
    let round_trip = Config::from_words(&good.0).unwrap();
    assert_eq!(round_trip.0, good.0);

    assert!(Config::from_message_bytes(b"not a capnp message!").is_err());
    // A truncated message fails too.
    let bytes = bytes_from_words(&good.0);
    assert!(Config::from_message_bytes(&bytes[..bytes.len() - 8]).is_err());
}
