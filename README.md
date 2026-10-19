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

Rate limiting for the stream subsystem: `limit_req` the way the http block
has always had it, for TCP and UDP.

nginx ships `limit_conn` for stream, which caps how many connections are
open at once, and `limit_req` only for http.  Nothing in the open source
server limits how *fast* connections arrive at a stream listener, so a
mail, database or VPN front end can be hammered with new connections as
fast as the network allows while every one of them stays well inside the
connection limit.

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

Sets up the shared memory zone that keeps the state, exactly as the http
directive of the same name does.  The key is any stream variable or a
combination of them, usually `$binary_remote_addr`.  The rate is given as
`r/s` or `r/m`; a zone smaller than eight pages is refused.

### `limit_req`

**syntax:** `limit_req zone=<name> [burst=<number>] [nodelay | delay=<number>]`
**context:** `stream`, `server`

Applies a zone.  Connections beyond the rate are refused unless `burst`
allows them to queue; `nodelay` lets the whole burst through at once,
`delay=N` lets the first N of it through at once and holds the rest back
until the rate allows them.  Several zones may be applied to one server,
and they are then all consulted.

### `limit_req_log_level`

**syntax:** `limit_req_log_level info | notice | warn | error`
**default:** `error`
**context:** `stream`, `server`

The level a refusal is logged at.  A delayed connection is logged one level
below that.

### `limit_req_dry_run`

**syntax:** `limit_req_dry_run on | off`
**default:** `off`
**context:** `stream`, `server`

Counts and logs as usual but lets everything through, so a rate can be
measured against real traffic before it is enforced.

## How it behaves

The module runs in the **preaccess phase**, before the connection reaches
the content phase: nothing is proxied, nothing is sent upstream, and a
refused connection is closed without a byte of payload having been read.

A refusal is logged as

```
limiting connections by zone "perip", excess: 1.000
```

and a dry run says `dry run limiting connections` instead.

The arithmetic is the leaky bucket nginx uses in its own `limit_req` for
http, so a rate configured here means the same thing it means there.

## Building

The module needs a stream enabled nginx:

```sh
./configure --with-stream --add-module=/path/to/nginx-stream-limit-req-module
```

or, as a loadable module,

```sh
./configure --with-stream \
    --add-dynamic-module=/path/to/nginx-stream-limit-req-module
```

and then `load_module modules/ngx_stream_limit_req_module.so;` in the main
context.  A build without `--with-stream` is refused at configure time
rather than failing at the link step.

## Testing

```sh
ci/build.sh 1.30.5 /tmp/nginx-test
TEST_NGINX_BINARY=/tmp/nginx-test/sbin/nginx prove -r t/
ci/rate.sh /tmp/nginx-test/sbin/nginx
ci/reload.sh /tmp/nginx-test/sbin/nginx
```

`t/` covers the configuration parser, `ci/rate.sh` opens real sockets and
counts what the limit lets through, and `ci/reload.sh` checks that the zone
survives a reload.

## License

BSD 2-Clause.  See [LICENSE](LICENSE).  The leaky bucket arithmetic follows
nginx's own `ngx_http_limit_req_module`, Copyright (C) Igor Sysoev,
Copyright (C) Nginx, Inc., under the same licence.
