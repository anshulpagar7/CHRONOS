# ADR-010: A hand-rolled HTTP/JSON layer instead of a web framework

**Status:** Accepted · **Date:** Day 5

## Context

Phase 5 needs a REST API and a dashboard over the running system. The
obvious move is a framework (Crow, Drogon, cpp-httplib); Crow was the
initial plan.

## Decision

Write the layer by hand: `chronos::api` contains a ~250-line strict JSON
parser/writer, an HTTP/1.1 server (blocking accept feeding a fixed worker
pool, one request per connection), and a pattern router
(`/api/jobs/{id}`). The dashboard is a single static, dependency-free
HTML file the daemon serves itself.

Reasons, in order:

1. **The needs are tiny and fixed.** Seven JSON routes plus static files,
   on localhost or inside a container. No TLS termination, keep-alive,
   streaming, or content negotiation — the features frameworks exist for.
2. **Dependency cost is real.** Crow drags in standalone ASIO; both are
   FetchContent'd, compiled, and become part of every CI matrix build
   (including TSan, where third-party code produces third-party
   suppressions). CHRONOS core is dependency-free; keeping the API layer
   the same preserves a property worth advertising.
3. **It is testable to the same standard as the core.** Request parsing
   and routing are pure functions with unit tests; the whole stack has a
   raw-TCP integration suite against a real server on an ephemeral port.
   A framework would make those tests *weaker* (testing the framework's
   parser, not ours).

Deliberate limits (documented in the header): `Connection: close` only,
16 KB header / 1 MB body caps, no chunked encoding, basic-plane-only
`\u` escapes in JSON, numbers as doubles. Each is fine for a control
plane and would be revisited the moment this fronted untrusted traffic
at volume.

## Consequences

- Zero new dependencies; the CI matrix build time is unchanged.
- The API layer is ~900 lines including tests — comparable to the
  integration glue a framework would have required anyway.
- Trade-off accepted: no HTTPS. Production deployments would terminate
  TLS in front (as is standard for control planes); the README says so.
- The dashboard needs no toolchain: no node_modules, no build step,
  `curl` and a browser are the entire client stack.
