// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Links from a span to other spans, `tracing::SpanLink` and `tracing::SpanLinks`.

use worker_interface_capnp::trace::span_link;
use worker_interface_capnp::trace::span_links;

use crate::Attribute;
use crate::SpanContext;
use crate::attribute::list_len;

/// A link to another span. Its ids may be zero when the attributes carry the relationship.
#[derive(Clone, Debug, PartialEq)]
pub struct SpanLink {
    pub context: SpanContext,
    pub attributes: Vec<Attribute>,
    pub dropped_attributes_count: u32,
}

impl SpanLink {
    pub fn from_capnp(reader: span_link::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self {
            context: SpanContext::from_capnp(reader.get_context()?)?,
            attributes: reader
                .get_attributes()?
                .iter()
                .map(Attribute::from_capnp)
                .collect::<capnp::Result<_>>()?,
            dropped_attributes_count: reader.get_dropped_attributes_count(),
        })
    }

    pub fn to_capnp(&self, mut builder: span_link::Builder<'_>) {
        self.context.to_capnp(builder.reborrow().init_context());
        let mut list = builder
            .reborrow()
            .init_attributes(list_len(self.attributes.len()));
        for (i, attribute) in (0..).zip(&self.attributes) {
            attribute.to_capnp(list.reborrow().get(i));
        }
        builder.set_dropped_attributes_count(self.dropped_attributes_count);
    }
}

/// The links of a span, which a tail stream reports as one event.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SpanLinks(pub Vec<SpanLink>);

impl SpanLinks {
    pub fn from_capnp(reader: span_links::Reader<'_>) -> capnp::Result<Self> {
        Ok(Self(
            reader
                .get_links()?
                .iter()
                .map(SpanLink::from_capnp)
                .collect::<capnp::Result<_>>()?,
        ))
    }

    pub fn to_capnp(&self, builder: span_links::Builder<'_>) {
        let mut list = builder.init_links(list_len(self.0.len()));
        for (i, link) in (0..).zip(&self.0) {
            link.to_capnp(list.reborrow().get(i));
        }
    }
}

#[cfg(test)]
#[path = "link-test.rs"]
mod tests;
