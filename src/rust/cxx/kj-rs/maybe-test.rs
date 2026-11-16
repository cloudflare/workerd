use super::repr::KjMaybe;

const VALUE: &str = "a value long enough to live on the heap";

#[test]
fn test_drop_some() {
    let maybe = KjMaybe::Some(String::from(VALUE));
    assert!(maybe.is_some());
    drop(maybe);
}

#[test]
fn test_drop_none() {
    let maybe = KjMaybe::<String>::None;
    assert!(maybe.is_none());
    drop(maybe);
}

#[test]
fn test_into_option_takes_the_value() {
    let some: Option<String> = KjMaybe::Some(String::from(VALUE)).into();
    assert_eq!(some.as_deref(), Some(VALUE));
    let none: Option<String> = KjMaybe::<String>::None.into();
    assert_eq!(none, None);
}
