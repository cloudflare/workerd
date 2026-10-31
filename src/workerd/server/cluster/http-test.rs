// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use tokio::io::AsyncReadExt;
use tokio::io::AsyncWriteExt;
use tokio::net::TcpListener;

use super::*;

#[test]
fn a_body_held_in_memory_is_capped() {
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    rt.block_on(async {
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let port = listener.local_addr().unwrap().port();
        tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let _ = stream.read(&mut [0; 4096]).await;
            let size = MAX_BODY + 1;
            let head = format!("HTTP/1.1 200 OK\r\ncontent-length: {size}\r\n\r\n");
            stream.write_all(head.as_bytes()).await.unwrap();
            let _ = stream.write_all(&vec![0; size]).await;
        });
        let http = Http::new().unwrap();
        let wait = Duration::from_secs(10);
        let url = format!("http://127.0.0.1:{port}/");
        let payload = Payload::Bytes(bytes::Bytes::new());
        let sent = http.send("GET".into(), url, vec![], payload, None, (wait, wait));
        assert_eq!(sent.await.err().as_deref(), Some("response body too large"));
    });
}
