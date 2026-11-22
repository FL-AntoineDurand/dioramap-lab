# Security

## Deployment model

`dioramap-processor serve` is a **lab tool**. It runs pipelines that download data
and use CPU, disk and network on behalf of any connected client.

- By default it binds to **127.0.0.1** and has **no authentication**.
- To expose it, use `--bind <address>` (or `--bind "*"`) **together with**
  `--token <secret>` (≥ 16 characters, or `DIORAMAP_TOKEN`). Clients must then
  connect with `?token=<secret>`. Connections without a valid token are rejected
  before any message is dispatched.
- The token is a single shared secret, not user authentication. Put the server
  behind a TLS-terminating reverse proxy if it is reachable from untrusted networks.

## Reporting a vulnerability

Please report vulnerabilities **privately** through GitHub's
[private vulnerability reporting](https://github.com/FL-AntoineDurand/dioramap-lab/security/advisories/new),
not in public issues. Fixes are best-effort.
