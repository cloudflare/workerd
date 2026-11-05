// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `bucket` service (`ClusterBucket` in workerd.capnp): a directory of
//! the cluster's bucket, served over HTTP as GET, PUT and DELETE on files.

use std::pin::Pin;
use std::rc::Rc;
use std::time::SystemTime;

use cluster::Bucket;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::Headers;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service as HttpService;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjOwn;
use worker::AlarmResult;
use worker::Interface;
use worker::ScheduledResult;
use workerd_capnp::cluster_bucket;

use crate::Result;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::cluster::ClusterNode;
use crate::config::Factory;
use crate::config::text;
use crate::services::disk::path_segments;
use crate::services::header_table;
use crate::services::not_transferable;
use crate::services::send_error;
use crate::services::unsupported;

struct Shared {
    bucket: Bucket,
    factory: Rc<Factory>,
}

pub struct BucketService(Rc<Shared>);

pub fn make_bucket_service(
    conf: cluster_bucket::Reader<'_>,
    node: &ClusterNode,
    factory: &Rc<Factory>,
) -> Result<BucketService> {
    let path = text(conf.get_path())?;
    let url = format!("http://bucket/{path}");
    if path.is_empty() || path_segments(&url).is_none_or(|s| s.join("/") != path) {
        return Err(kj::failed!(
            "bucket path {path:?} must be `/`-separated names."
        ));
    }
    Ok(BucketService(Rc::new(Shared {
        bucket: node.cluster.services(&path),
        factory: Rc::clone(factory),
    })))
}

impl Channel for BucketService {
    fn start_request(&self, _metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        Ok(BucketRequest(Rc::clone(&self.0)).into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("Bucket services"))
    }
}

struct BucketRequest(Rc<Shared>);

async fn read_body(mut body: Pin<&mut AsyncInputStream>) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    let mut buffer = vec![0; 64 * 1024];
    loop {
        let read = body.as_mut().try_read(&mut buffer, 1).await?;
        if read == 0 {
            return Ok(out);
        }
        out.extend_from_slice(&buffer[..read]);
    }
}

#[async_trait::async_trait(?Send)]
impl HttpService for BucketRequest {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        _headers: HeadersRef<'a>,
        body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let none = Headers::new(header_table(&self.0.factory));
        let bucket = &self.0.bucket;
        let fail = |e: cluster::Error| kj::failed!("{e}");
        let Some(key) = path_segments(&String::from_utf8_lossy(url))
            .filter(|s| !s.is_empty())
            .map(|s| s.join("/"))
        else {
            return send_error(response, 404, "Not Found", &none).await;
        };
        match method {
            Method::GET => {
                let Some((value, _)) = bucket.get(&key).await.map_err(fail)? else {
                    return send_error(response, 404, "Not Found", &none).await;
                };
                let len = value.len() as u64;
                let mut out = response.send(200, "OK", &none, Some(len))?;
                out.write(&value).await
            }
            Method::PUT => {
                let value = read_body(body).await?;
                bucket.put(&key, value, &[]).await.map_err(fail)?;
                response.send(204, "No Content", &none, None)?;
                Ok(())
            }
            Method::DELETE => {
                bucket.delete(&key).await.map_err(fail)?;
                response.send(204, "No Content", &none, None)?;
                Ok(())
            }
            _ => send_error(response, 501, "Not Implemented", &none).await,
        }
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> Result<()> {
        Err(unsupported("Bucket services"))
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for BucketRequest {
    async fn run_scheduled(&mut self, _time: &SystemTime, _cron: &str) -> Result<ScheduledResult> {
        Err(unsupported("Bucket services"))
    }

    async fn run_alarm(&mut self, _time: &SystemTime, _retry_count: u32) -> Result<AlarmResult> {
        Err(unsupported("Bucket services"))
    }
}
