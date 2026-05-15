use std::pin::Pin;

use kj_rs::KjOwn;

use crate::OwnOrMut;
use crate::Result;

#[cxx::bridge(namespace = "kj::rust")]
pub mod ffi {
    unsafe extern "C++" {
        include!("workerd/rust/kj/ffi.h");

        type AsyncInputStream;
        type AsyncIoStream;
        type AsyncOutputStream;

        async fn async_output_stream_write(
            this_: Pin<&mut AsyncOutputStream>,
            buffer: &[u8],
        ) -> Result<()>;

        async fn async_output_stream_when_write_disconnected(
            this_: Pin<&mut AsyncOutputStream>,
        ) -> Result<()>;
    }
}

pub type AsyncInputStream = ffi::AsyncInputStream;
pub type AsyncIoStream = ffi::AsyncIoStream;

/// Owned-or-borrowed wrapper for `kj::AsyncOutputStream`.
pub struct AsyncOutputStream<'a>(OwnOrMut<'a, ffi::AsyncOutputStream>);

impl AsyncOutputStream<'_> {
    pub async fn write(&mut self, buffer: &[u8]) -> Result<()> {
        let stream = self.0.as_mut();
        ffi::async_output_stream_write(stream, buffer).await?;
        Ok(())
    }

    pub async fn when_write_disconnected(&mut self) -> Result<()> {
        let stream = self.0.as_mut();
        ffi::async_output_stream_when_write_disconnected(stream).await?;
        Ok(())
    }
}

impl From<KjOwn<ffi::AsyncOutputStream>> for AsyncOutputStream<'_> {
    fn from(value: KjOwn<ffi::AsyncOutputStream>) -> Self {
        Self(value.into())
    }
}

impl<'a> From<Pin<&'a mut ffi::AsyncOutputStream>> for AsyncOutputStream<'a> {
    fn from(value: Pin<&'a mut ffi::AsyncOutputStream>) -> Self {
        Self(value.into())
    }
}
