use std::pin::Pin;

use kj::Result;
use kj::http::CustomHeaderId;
use kj::http::HeadersRef;
use kj::io::AsyncIoStream;
use kj_rs::KjMaybe;

#[cxx::bridge(namespace = "kj::rust::tests")]
// The named `'a` on the mirrored `HttpConnectSettings<'a>` shared struct cannot be elided to
// `'_` in a struct definition and is required by the cxx::bridge dialect.
#[allow(clippy::elidable_lifetime_names)]
pub mod ffi {
    #[namespace = "kj::rust"]
    unsafe extern "C++" {
        include!("kj-rs-http/ffi.h");
        type HttpService = kj::http::ffi::HttpService;
        type HttpHeaders = kj::http::ffi::HttpHeaders;
        type HttpHeaderId = kj::http::ffi::HttpHeaderId;
        type AsyncIoStream = kj::io::ffi::AsyncIoStream;
        type ConnectResponse = kj::http::ffi::ConnectResponse;
        type TlsStarterCallback = kj::http::ffi::TlsStarterCallback;
    }

    /// Mirror of `kj::http::ffi::HttpConnectSettings` (shared structs cannot be aliased across
    /// bridges; the C++ definition is include-guarded, so the duplicate is benign).
    #[namespace = "kj::rust"]
    struct HttpConnectSettings<'a> {
        use_tls: bool,
        tls_starter: KjMaybe<Pin<&'a mut TlsStarterCallback>>,
    }

    extern "Rust" {
        /// Forward a connect() call to `service` from Rust, passing `settings` through. This
        /// exercises the C++ -> Rust -> C++ round-trip for HttpConnectSettings (including the
        /// outbound tls_starter).
        async unsafe fn connect_through_rust<'a>(
            service: Pin<&'a mut HttpService>,
            host: &'a [u8],
            headers: &'a HttpHeaders,
            connection: Pin<&'a mut AsyncIoStream>,
            response: Pin<&'a mut ConnectResponse>,
            settings: HttpConnectSettings<'a>,
        ) -> Result<()>;

        /// Look up a header value by HttpHeaderId, returning the value if present.
        /// This exercises the C++ -> Rust -> C++ round-trip for HttpHeaderId.
        unsafe fn get_header_value_via_id<'a>(
            headers: &'a HttpHeaders,
            id: &HttpHeaderId,
        ) -> KjMaybe<&'a [u8]>;

        /// Look up a header value by name via `HeadersRef::get_by_name`, returning the value if
        /// present. This exercises the C++ -> Rust -> C++ round-trip for name-based lookup.
        unsafe fn get_header_value_via_name<'a>(
            headers: &'a HttpHeaders,
            name: &str,
        ) -> KjMaybe<&'a [u8]>;

        /// Clear all headers via the `clear_headers` FFI shim. This exercises the
        /// C++ -> Rust -> C++ round-trip for clearing headers.
        fn clear_headers_via_rust(headers: Pin<&mut HttpHeaders>);

        /// Receive an array of HttpHeaderIdpointers, convert to &[HttpHeaderIdRef] via
        /// from_ptr_slice, look up each header, and assert all are present.
        /// This exercises passing a kj::ArrayPtr<const kj::HttpHeaderId> into Rust.
        unsafe fn assert_header_ids_present(headers: &HttpHeaders, ids: &[*const HttpHeaderId]);
    }
}

async fn connect_through_rust<'a>(
    service: Pin<&'a mut ffi::HttpService>,
    host: &'a [u8],
    headers: &'a ffi::HttpHeaders,
    connection: Pin<&'a mut AsyncIoStream>,
    response: Pin<&'a mut ffi::ConnectResponse>,
    settings: ffi::HttpConnectSettings<'a>,
) -> Result<()> {
    // Rebuild the settings as the main bridge's identical struct (see the bridge definition),
    // then forward to kj::HttpService::connect the same way CxxService does.
    let settings = kj::http::ffi::HttpConnectSettings {
        use_tls: settings.use_tls,
        tls_starter: settings.tls_starter,
    };
    kj::http::ffi::connect(service, host, headers, connection, response, settings).await?;
    Ok(())
}

fn get_header_value_via_id<'a>(
    headers: &'a ffi::HttpHeaders,
    id: &ffi::HttpHeaderId,
) -> KjMaybe<&'a [u8]> {
    // SAFETY: headers is a valid HttpHeaders ref and id is a valid HttpHeaderId from C++.
    unsafe { kj::http::ffi::get_header_by_id(headers, id) }
}

fn get_header_value_via_name<'a>(headers: &'a ffi::HttpHeaders, name: &str) -> KjMaybe<&'a [u8]> {
    HeadersRef::from(headers).get_by_name(name).into()
}

fn clear_headers_via_rust(headers: Pin<&mut ffi::HttpHeaders>) {
    kj::http::ffi::clear_headers(headers);
}

/// # Safety
///
/// Each pointer in `ids` must be non-null and point to a valid, live `HttpHeaderId`.
unsafe fn assert_header_ids_present(headers: &ffi::HttpHeaders, ids: &[*const ffi::HttpHeaderId]) {
    let headers_ref = HeadersRef::from(headers);
    // SAFETY: All pointers in ids are valid, as guaranteed by the unsafe fn contract.
    let id_refs = unsafe { CustomHeaderId::from_ptr_slice(ids) };
    for (i, &id_ref) in id_refs.iter().enumerate() {
        let value = headers_ref.get_by_id(id_ref);
        assert!(
            value.is_some(),
            "expected header at index {i} to be present, but got None"
        );
    }
}
