# src/opentelemetry/

## OVERVIEW

The OpenTelemetry protocol (OTLP) schemas for the signals workerd exports, vendored from
[opentelemetry-proto](https://github.com/open-telemetry/opentelemetry-proto). The runtime does not
use them to export: `src/rust/otlp` encodes its requests through its own prost structs
(`src/rust/otlp/proto.rs`). The C++ classes generated here, `//src/opentelemetry:otlp_cc_proto`,
are for tests that decode those requests, and for embedders' tests that do the same.

## STRUCTURE

The paths mirror upstream's `opentelemetry/proto/`, so the files keep their upstream imports:

- `proto/collector/{trace,logs}/v1/`: the export requests, `ExportTraceServiceRequest` and
  `ExportLogsServiceRequest`.
- `proto/{trace,logs}/v1/`: spans and log records.
- `proto/{common,resource}/v1/`: attributes and resources, shared by both.

Only the signals workerd exports are here; metrics and profiles are left out.

## MAINTENANCE

- Each file is byte-identical to upstream below its `OBTAINED FROM` header, which names the
  upstream commit and release tag (currently v1.11.1). Don't edit the schemas themselves.
- To update, copy the files from a newer release tag, keep the header, and update the commit, tag
  and date in it. All files come from the same release.
- OTLP's protobuf encoding is stable, so an update is only needed for a newly introduced field
  that a test needs to decode. When the exporter starts writing a new field, add it to
  `src/rust/otlp/proto.rs` with the field number from these files.
