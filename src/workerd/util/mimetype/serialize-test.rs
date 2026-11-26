use super::*;

struct P(&'static [u8], &'static [u8]);

impl Param for P {
    fn name(&self) -> &[u8] {
        self.0
    }

    fn value(&self) -> &[u8] {
        self.1
    }
}

fn to_string(type_: &[u8], subtype: &[u8], params: &[P]) -> Vec<u8> {
    let mut out = vec![0; serialized_len(type_, subtype, params)];
    assert_eq!(serialize(type_, subtype, params, &mut out), out.len());
    out
}

fn params_to_string(params: &[P]) -> Vec<u8> {
    let mut out = vec![0; params_serialized_len(params)];
    assert_eq!(serialize_params(params, &mut out), out.len());
    out
}

#[test]
fn essence_only() {
    assert_eq!(to_string(b"text", b"plain", &[]), b"text/plain");
    assert_eq!(params_to_string(&[]), b"");
}

#[test]
fn params() {
    let params = [P(b"charset", b"utf-8"), P(b"a", b"b")];
    assert_eq!(
        to_string(b"text", b"plain", &params),
        b"text/plain;charset=utf-8;a=b"
    );
    assert_eq!(params_to_string(&params), b"charset=utf-8;a=b");
}

#[test]
fn boundary_gets_a_space() {
    let params = [P(b"a", b"b"), P(b"boundary", b"xyz")];
    assert_eq!(
        to_string(b"multipart", b"form-data", &params),
        b"multipart/form-data;a=b; boundary=xyz"
    );
    assert_eq!(params_to_string(&[P(b"boundary", b"x")]), b" boundary=x");
}

#[test]
fn quoting() {
    assert_eq!(params_to_string(&[P(b"a", b"")]), b"a=\"\"");
    assert_eq!(params_to_string(&[P(b"a", b"b c")]), b"a=\"b c\"");
    assert_eq!(
        params_to_string(&[P(b"a", b"b\"c\\d")]),
        b"a=\"b\\\"c\\\\d\""
    );
    assert_eq!(params_to_string(&[P(b"a", b"\\")]), b"a=\\");
    assert_eq!(params_to_string(&[P(b"a", b"\xe9")]), b"a=\"\xe9\"");
    assert_eq!(params_to_string(&[P(b"a", b"b\\c")]), b"a=b\\c");
}

#[test]
fn reports_a_length_that_disagrees() {
    let mut short = [0u8; 4];
    assert_eq!(
        serialize(b"text", b"plain", &[P(b"a", b"b")], &mut short),
        14
    );
    assert_eq!(&short, b"text");
    let mut long = [0u8; 8];
    assert_eq!(serialize_params(&[P(b"a", b"b")], &mut long), 3);
}
