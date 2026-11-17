// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The headers a rewriter returns share the header table of the request's headers, so they cannot
// outlive them.
use kj::http::HeaderTable;
use kj::http::Headers;
use workerd_server::services::rewriter::HttpRewriter;

pub fn t(rewriter: &HttpRewriter) {
    let rewritten = {
        let table = HeaderTable::builtin();
        let headers = Headers::new(&table); //~ ERROR E0597
        rewriter.rewrite_outgoing_request(&table, "http://h/", headers.as_ref(), None) //~ ERROR E0597
    };
    drop(rewritten);
}
