#!/bin/sh
#
# Drive the limit against a real stream server and count what gets through.
#
# A test suite that speaks http cannot reach this module: it works on
# connections, before a byte of payload has arrived, and the thing it
# refuses is a connection and not a request.  So the client here is a few
# lines of perl that open sockets and report what came back.
#
# usage: ci/rate.sh <nginx binary>
#
# Set RATE_LOAD_MODULE to the path of ngx_stream_limit_req_module.so to
# test a loadable build.

set -eu

NGINX=${1:?path to the nginx binary missing}
WORK=${CI_WORK:-$(cd "$(dirname "$0")/.." && pwd)/ci-work}/rate
P1=${RATE_PORT:-18530}
P2=$((P1 + 1))
P3=$((P1 + 2))
P4=$((P1 + 3))

command -v perl > /dev/null || { echo "perl(1) is needed" >&2; exit 2; }

rm -rf "$WORK"
mkdir -p "$WORK/conf" "$WORK/logs"

fail=0
note() {
	echo "  $1" >&2
	fail=1
}

load=
[ -n "${RATE_LOAD_MODULE:-}" ] && load="load_module $RATE_LOAD_MODULE;"

cat > "$WORK/conf/nginx.conf" <<CONF
$load
daemon on;
master_process on;
worker_processes 1;
pid $WORK/logs/nginx.pid;
error_log $WORK/logs/error.log info;
events { worker_connections 64; }
stream {
    limit_req_zone \$binary_remote_addr zone=plain:1m   rate=1r/s;
    limit_req_zone \$binary_remote_addr zone=bursty:1m  rate=1r/s;
    limit_req_zone \$binary_remote_addr zone=waiting:1m rate=5r/s;
    limit_req_zone \$binary_remote_addr zone=dry:1m     rate=1r/s;

    server {
        listen 127.0.0.1:$P1;
        limit_req zone=plain;
        return "ok1";
    }

    server {
        listen 127.0.0.1:$P2;
        limit_req zone=bursty burst=5 nodelay;
        return "ok2";
    }

    server {
        listen 127.0.0.1:$P3;
        limit_req zone=waiting burst=5 delay=2;
        return "ok3";
    }

    server {
        listen 127.0.0.1:$P4;
        limit_req zone=dry;
        limit_req_dry_run on;
        return "ok4";
    }
}
CONF

cat > "$WORK/client.pl" <<'PERL'
use strict;
use warnings;
use IO::Socket::INET;
use Time::HiRes qw(time);

# One run: open $n connections to $port as fast as the loop allows, read
# whatever comes back, and print how many answered and how long it took.
# A connection the module refused is closed before any payload, so it
# reports an empty read -- which is exactly the difference we measure.

my ($port, $n) = @ARGV;
my $start = time;
my $answered = 0;

for my $i (1 .. $n) {
    my $s = IO::Socket::INET->new(PeerAddr => '127.0.0.1', PeerPort => $port,
                                  Proto => 'tcp', Timeout => 5);
    if (!$s) { next }

    $s->autoflush(1);
    my $buf = '';
    eval {
        local $SIG{ALRM} = sub { die "timeout\n" };
        alarm 5;
        $s->sysread($buf, 64);
        alarm 0;
    };
    $answered++ if defined $buf && length $buf;
    close $s;
}

printf "%d %.3f\n", $answered, time - $start;
PERL

cleanup() {
	# every line survives a failure of its own: this runs from an EXIT
	# trap under set -e, where a failed test aborts the function and
	# makes its status the status of the whole script
	if [ -f "$WORK/logs/nginx.pid" ]; then
		kill -QUIT "$(cat "$WORK/logs/nginx.pid")" 2>/dev/null || true
	fi
	sleep 1
}
trap cleanup EXIT INT TERM

"$NGINX" -p "$WORK" -c "$WORK/conf/nginx.conf"
sleep 1

run() {
	perl "$WORK/client.pl" "$1" "$2"
}

# The client prints "<answered> <seconds>".  Split by hand rather than with
# set --, which shellcheck rightly dislikes: an unquoted substitution there
# would split on anything the client ever printed with a space in it.
answered=0
elapsed=0

measure() {
	out=$(run "$1" "$2")
	answered=${out%% *}
	elapsed=${out##* }
}

echo "--- rate=1r/s, no burst: one connection gets through ---"
measure "$P1" 5
echo "  5 connections, $answered answered in $elapsed s"
[ "$answered" -eq 1 ] || note "expected exactly 1 answer, got $answered"

echo "--- rate=1r/s, burst=5 nodelay: six get through at once ---"
measure "$P2" 10
echo "  10 connections, $answered answered in $elapsed s"
[ "$answered" -eq 6 ] || note "expected 6 answers, got $answered"
# nodelay means they are not held back: the whole run has to be quick
awk -v t="$elapsed" 'BEGIN { exit (t < 1.0) ? 0 : 1 }' ||
	note "nodelay still took $elapsed s"

echo "--- rate=5r/s, burst=5 delay=2: nothing is refused, some is held ---"
measure "$P3" 6
echo "  6 connections, $answered answered in $elapsed s"
[ "$answered" -eq 6 ] || note "expected 6 answers, got $answered"
# the third connection onwards waits 1/5 s each, so the run cannot be
# instant; without the delay path this is well under a tenth of a second
awk -v t="$elapsed" 'BEGIN { exit (t > 0.4) ? 0 : 1 }' ||
	note "the delayed run took only $elapsed s, nothing was held back"

echo "--- dry run: everything gets through, the log says it would not ---"
measure "$P4" 5
echo "  5 connections, $answered answered in $elapsed s"
[ "$answered" -eq 5 ] || note "a dry run must not refuse anything, got $answered of 5"
grep -q 'dry run limiting connections by zone "dry"' "$WORK/logs/error.log" ||
	note "the dry run left no trace in the error log"

echo "--- the refusals are logged ---"
grep -q 'limiting connections by zone "plain"' "$WORK/logs/error.log" ||
	note "no log line for the refused connections"

echo "--- nothing alarming in the error log ---"
if grep -qE '\[(alert|emerg)\]' "$WORK/logs/error.log"; then
	note "alert or emerg in the error log"
	grep -E '\[(alert|emerg)\]' "$WORK/logs/error.log" | head -3 >&2
else
	echo "  clean"
fi

[ "$fail" -eq 0 ] || { echo "ci/rate.sh: failed" >&2; exit 1; }
echo "ci/rate.sh: ok"
