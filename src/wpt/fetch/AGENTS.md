# src/wpt/fetch/

WPT `fetch/api` runs in two cells, one per streams implementation:

| Target | Config | Streams implementation |
| --- | --- | --- |
| `//src/wpt:fetch/api` | `api-test.ts` | legacy C++ (`src/workerd/api/streams/`) |
| `//src/wpt:fetch/api-ts` | `api-test-ts.ts` | TypeScript (`src/per_isolate/webstreams/`), via `typescript_implemented_streams` and the `per-isolate-javascript-bootstrap` autogate |

Both cells pin the same compatibility flags otherwise
(`strip_bom_in_read_all_text`). Most entries in either config are fetch API
divergences that apply to both implementations (missing CORS, cache modes,
redirect handling, header validation, unimplemented methods, ...). They are
duplicated in both configs and kept identical.

## Divergence ledger (C++ streams vs TypeScript streams)

Subtests whose result depends on the streams implementation. A row here
means the two configs differ for that subtest: it is an `expectedFailures`
or `disabledTests` entry in one config and not in the other. When a row is
added, the `comment` of the differing entry names its ledger number, and
the row says whether the difference is intentional (citing the
`src/tests/streams/*/AGENTS.md` or `src/tests/node/*/AGENTS.md` ledger entry
that pins it) or a defect.

| # | WPT file / subtest | C++ streams | TypeScript streams | Intentional? |
| --- | --- | --- | --- | --- |
| — | none: both configs currently carry identical expectations | | | |

## Updating the configs

- A change to a fetch divergence that is independent of the streams
  implementation goes into **both** configs.
- A subtest that passes or fails in only one cell gets a ledger row. Before
  marking a TS-only failure as expected, check it against the streams
  suites' ledgers; a failure that no ledger entry explains is a defect to
  fix, not an expectation to record.
- WPT targets have `@` and `@all-autogates` variants only (no
  `@all-compat-flags`); results must match in both.
