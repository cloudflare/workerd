use super::*;

/// A config with one service named "main".
fn one_service_config() -> Config {
    let mut message = capnp::message::Builder::new_default();
    let config = message.init_root::<config::Builder>();
    config.init_services(1).get(0).set_name("main");
    Config(words_from_bytes(&capnp::serialize::write_message_to_words(
        &message,
    )))
}

#[test]
fn config_bytes_are_checked() {
    let good = one_service_config();
    let round_trip = Config::from_words(&good.0).unwrap();
    assert_eq!(round_trip.0, good.0);

    assert!(Config::from_message_bytes(b"not a capnp message!").is_err());
    // A truncated message fails too.
    let bytes = bytes_from_words(&good.0);
    assert!(Config::from_message_bytes(&bytes[..bytes.len() - 8]).is_err());
}
