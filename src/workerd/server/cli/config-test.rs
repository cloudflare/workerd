use super::*;

fn read(config: &Config) -> capnp::message::Reader<capnp::serialize::OwnedSegments> {
    capnp::serialize::read_message(bytes_from_words(&config.0).as_slice(), reader_options())
        .unwrap()
}

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

/// A config with `shutdownDelayMs` set to `delay`.
fn shutdown_delay_config(delay: u32) -> Config {
    let mut message = capnp::message::Builder::new_default();
    let mut root = message.init_root::<config::Builder>();
    root.reborrow().init_services(1).get(0).set_name("main");
    root.set_shutdown_delay_ms(delay);
    Config(words_from_bytes(&capnp::serialize::write_message_to_words(
        &message,
    )))
}

/// The `shutdownDelayMs` of `config`.
fn shutdown_delay_of(config: &Config) -> u32 {
    let message = read(config);
    let root = message.get_root::<config::Reader>().unwrap();
    // The rest of the config is kept.
    assert_eq!(
        root.get_services().unwrap().get(0).get_name().unwrap(),
        "main"
    );
    root.get_shutdown_delay_ms()
}

/// An environment where `SHUTDOWN_DELAY_MS` is `value`, if any.
fn shutdown_delay_env(value: Option<&str>) -> impl Fn(&str) -> Option<OsString> + '_ {
    move |name| {
        assert_eq!(name, SHUTDOWN_DELAY_MS);
        value.map(OsString::from)
    }
}

#[test]
fn shutdown_delay_unset_or_blank_in_environment_stays_zero() {
    for value in [None, Some(""), Some(" \n")] {
        let config = shutdown_delay_config(0)
            .with_shutdown_delay_environment(shutdown_delay_env(value))
            .unwrap();
        assert_eq!(shutdown_delay_of(&config), 0, "{value:?}");
    }
}

#[test]
fn shutdown_delay_comes_from_environment() {
    for (value, delay) in [
        ("20000", 20_000),
        ("0", 0),
        ("3600000", 3_600_000),
        // Trailing whitespace, such as a file's final newline, is trimmed.
        ("20000\n", 20_000),
        ("20000 \t", 20_000),
    ] {
        let config = shutdown_delay_config(0)
            .with_shutdown_delay_environment(shutdown_delay_env(Some(value)))
            .unwrap();
        assert_eq!(shutdown_delay_of(&config), delay, "{value:?}");
    }
}

#[test]
fn shutdown_delay_in_config_wins_over_environment() {
    // The variable is not read, so not even an unusable value is an error.
    for value in ["20000", "abc"] {
        let config = shutdown_delay_config(5000)
            .with_shutdown_delay_environment(shutdown_delay_env(Some(value)))
            .unwrap();
        assert_eq!(shutdown_delay_of(&config), 5000, "{value}");
    }
}

#[test]
fn unusable_shutdown_delay_in_environment_is_an_error() {
    for value in [
        "abc",
        "-1",
        "+20",
        "1.5",
        "20s",
        " 20",
        "2 0",
        "3600001",
        "99999999999",
    ] {
        let Err(error) = shutdown_delay_config(0)
            .with_shutdown_delay_environment(shutdown_delay_env(Some(value)))
        else {
            panic!("{value:?} accepted");
        };
        assert_eq!(
            error,
            format!(
                "WORKERD_SHUTDOWN_DELAY_MS={value}: not a whole number of milliseconds from 0 \
                 to 3600000 (an hour)."
            )
        );
    }
}

#[cfg(unix)]
#[test]
fn shutdown_delay_in_environment_that_is_not_utf8_is_an_error() {
    use std::os::unix::ffi::OsStringExt;

    let var = |name: &str| {
        assert_eq!(name, SHUTDOWN_DELAY_MS);
        Some(OsString::from_vec(b"20\xff".to_vec()))
    };
    let Err(error) = shutdown_delay_config(0).with_shutdown_delay_environment(var) else {
        panic!("accepted");
    };
    assert_eq!(
        error,
        "WORKERD_SHUTDOWN_DELAY_MS=20\u{fffd}: not UTF-8 text."
    );
}

#[test]
fn shutdown_delay_above_an_hour_in_config_is_an_error() {
    let config = shutdown_delay_config(3_600_000)
        .with_shutdown_delay_environment(shutdown_delay_env(None))
        .unwrap();
    assert_eq!(shutdown_delay_of(&config), 3_600_000);

    let Err(error) =
        shutdown_delay_config(20_000_000).with_shutdown_delay_environment(shutdown_delay_env(None))
    else {
        panic!("accepted");
    };
    assert_eq!(
        error,
        "Config.shutdownDelayMs = 20000000: more than 3600000 (an hour)."
    );
}
