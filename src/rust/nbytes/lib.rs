#[cxx::bridge(namespace = "workerd::rust::nbytes")]
mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/nbytes/ffi.h");

        fn base64_decode(input: &[u8]) -> Vec<u8>;
    }
}

pub fn base64_decode(input: &[u8]) -> Vec<u8> {
    ffi::base64_decode(input)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_base64_decode() {
        let input = b"SGVsbG8gV29ybGQ=";
        let result = base64_decode(input);
        assert_eq!(result, b"Hello World");
    }

    #[test]
    fn test_base64_decode_no_padding() {
        let input = b"SGVsbG8";
        let result = base64_decode(input);
        assert_eq!(result, b"Hello");
    }

    #[test]
    fn test_base64_decode_empty() {
        let result = base64_decode(b"");
        assert!(result.is_empty());
    }
}
