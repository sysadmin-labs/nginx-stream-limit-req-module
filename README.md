# nginx-stream-limit-req-module

[![Build & Test][build-test-badge]][build-test-link]
[![Rate][rate-badge]][rate-link]
[![FreeBSD][freebsd-badge]][freebsd-link]
[![A/UBSan][sanitizers-badge]][sanitizers-link]
[![Valgrind][valgrind-badge]][valgrind-link]
[![Reload][reload-badge]][reload-link]
[![CodeQL][codeql-badge]][codeql-link]
[![Lint][lint-badge]][lint-link]

[build-test-badge]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/build-test.yml/badge.svg
[build-test-link]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/build-test.yml
[rate-badge]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/rate.yml/badge.svg
[rate-link]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/rate.yml
[freebsd-badge]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/freebsd.yml/badge.svg
[freebsd-link]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/freebsd.yml
[sanitizers-badge]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/sanitizers.yml/badge.svg
[sanitizers-link]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/sanitizers.yml
[valgrind-badge]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/valgrind.yml/badge.svg
[valgrind-link]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/valgrind.yml
[reload-badge]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/reload.yml/badge.svg
[reload-link]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/reload.yml
[codeql-badge]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/codeql.yml/badge.svg
[codeql-link]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/codeql.yml
[lint-badge]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/lint.yml/badge.svg
[lint-link]: https://github.com/sysadmin-labs/nginx-stream-limit-req-module/actions/workflows/lint.yml

`limit_req` for the stream subsystem: rate limiting of incoming TCP and UDP
connections, with the directives, the parameters and the leaky bucket of the
http module of the same name.  `sync`, which belongs to nginx Plus, is not
part of it.

nginx ships `limit_conn` for stream, which caps how many connections are
open at once, and `limit_req` only for http.  Nothing in the open source
server limits the rate at which connections arrive at a stream listener, so
a mail, database or VPN front end can be fed new connections as fast as the
network allows while staying inside the connection limit.

## Synopsis

```nginx
stream {
    limit_req_zone $binary_remote_addr zone=perip:10m rate=5r/s;

    server {
        listen 25;
        limit_req zone=perip burst=10 delay=5;
        proxy_pass backend_smtp;
    }

    server {
        listen 53 udp;
        limit_req zone=perip burst=20 nodelay;
        proxy_pass backend_dns;
    }
}
```

## Directives

### `limit_req_zone`

**syntax:** `limit_req_zone <key> zone=<name>:<size> rate=<rate>`
**context:** `stream`

Defines the shared memory zone holding the state.  The key is any stream
variable or a combination of them, usually `$binary_remote_addr`.  The rate
is given in `r/s` or `r/m`.  A zone smaller than eight pages is rejected.

### `limit_req`

**syntax:** `limit_req zone=<name> [burst=<number>] [nodelay | delay=<number>]`
**context:** `stream`, `server`

Applies a zone.  Connections above the rate are refused unless `burst`
allows them to queue; `nodelay` passes the whole burst at once, `delay=N`
passes the first N of it and holds the rest until the rate allows them.
Several zones may be applied to one server and are then all consulted.

### `limit_req_log_level`

**syntax:** `limit_req_log_level info | notice | warn | error`
**default:** `error`
**context:** `stream`, `server`

Level a refusal is logged at.  A delayed connection is logged one level
below that.

### `limit_req_dry_run`

**syntax:** `limit_req_dry_run on | off`
**default:** `off`
**context:** `stream`, `server`

Counts and logs as usual but passes everything, so a rate can be measured
against production traffic before it is enforced.

## Variables

### `$limit_req_status`

What the module did with this connection: `PASSED`, `DELAYED`, `REJECTED`,
`DELAYED_DRY_RUN` or `REJECTED_DRY_RUN`.  Not found on a connection that
reached no `limit_req`.  A dry run is only visible through this variable and
the error log, so an `access_log` carrying it is what a measurement before
enforcement is read from.

## Behaviour

The handler runs in the preaccess phase, before the content phase: nothing
is proxied, and a refused connection is closed without a byte of payload
having been read.

A refusal is logged as

```
limiting connections by zone "perip", excess: 1.000
```

a connection held back by `burst` as

```
delaying connection, excess: 0.600, by zone "perip"
```

and a dry run writes `dry run` in front of either.

The rate arithmetic is the one `ngx_http_limit_req_module` uses, so a rate
configured here means what it means in an http block.

## Installation

The module requires a stream enabled nginx:

```sh
./configure --with-stream --add-module=/path/to/nginx-stream-limit-req-module
```

or as a loadable module

```sh
./configure --with-stream \
    --add-dynamic-module=/path/to/nginx-stream-limit-req-module
```

with `load_module modules/ngx_stream_limit_req_module.so;` in the main
context.  A build without `--with-stream` is rejected at configure time
rather than failing at the link step.

## Tests

```sh
ci/build.sh 1.30.5 /tmp/nginx-test
TEST_NGINX_BINARY=/tmp/nginx-test/sbin/nginx prove -r t/
ci/rate.sh /tmp/nginx-test/sbin/nginx
ci/reload.sh /tmp/nginx-test/sbin/nginx
```

`t/` covers the configuration parser, `ci/rate.sh` opens sockets and counts
what the limit passes, `ci/reload.sh` checks that the zone survives a
reload.

## License

BSD 2-Clause, see [LICENSE](LICENSE).  The rate arithmetic follows
`ngx_http_limit_req_module`, Copyright (C) Igor Sysoev, Copyright (C)
Nginx, Inc., under the same licence.
