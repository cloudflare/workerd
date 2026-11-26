use super::*;

#[test]
fn arrays_keep_their_elements_and_null_slots_in_order() {
    let mut array = AttributeArray::default();
    array.push(Some(Value::String("a".to_owned())));
    array.push(None);
    array.push(Some(Value::String("b".to_owned())));
    let mut attributes = Attributes::default();
    attributes.push("tags", array);
    attributes.push("empty", AttributeArray::default());

    let element = |value| proto::AnyValue { value };
    assert_eq!(
        attributes.into_proto(),
        vec![
            key_value(
                "tags",
                Value::Array(proto::ArrayValue {
                    values: vec![
                        element(Some(Value::String("a".to_owned()))),
                        element(None),
                        element(Some(Value::String("b".to_owned()))),
                    ],
                }),
            ),
            key_value("empty", Value::Array(proto::ArrayValue { values: vec![] })),
        ]
    );
}
