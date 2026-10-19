use Test::Nginx::Socket 'no_plan';

run_tests();

__DATA__

=== TEST 1: a limit without a zone is refused
--- main_config
stream {
    limit_req_zone $binary_remote_addr zone=t1:1m rate=1r/s;
    server {
        listen 127.0.0.1:18591;
        limit_req burst=5;
        return "x";
    }
}
--- config
    location = /t { return 200 ok; }
--- must_die
--- error_log
must have "zone" parameter



=== TEST 2: the same zone twice in one server is refused
--- main_config
stream {
    limit_req_zone $binary_remote_addr zone=t2:1m rate=1r/s;
    server {
        listen 127.0.0.1:18592;
        limit_req zone=t2;
        limit_req zone=t2;
        return "x";
    }
}
--- config
    location = /t { return 200 ok; }
--- must_die
--- error_log
is duplicate



=== TEST 3: a rate of zero is refused
--- main_config
stream {
    limit_req_zone $binary_remote_addr zone=t3:1m rate=0r/s;
    server {
        listen 127.0.0.1:18593;
        limit_req zone=t3;
        return "x";
    }
}
--- config
    location = /t { return 200 ok; }
--- must_die
--- error_log
invalid rate



=== TEST 4: a zone smaller than eight pages is refused
--- main_config
stream {
    limit_req_zone $binary_remote_addr zone=t4:4k rate=1r/s;
    server {
        listen 127.0.0.1:18594;
        limit_req zone=t4;
        return "x";
    }
}
--- config
    location = /t { return 200 ok; }
--- must_die
--- error_log
is too small



=== TEST 5: an unknown parameter is refused
--- main_config
stream {
    limit_req_zone $binary_remote_addr zone=t5:1m rate=1r/s;
    server {
        listen 127.0.0.1:18595;
        limit_req zone=t5 nonsense=1;
        return "x";
    }
}
--- config
    location = /t { return 200 ok; }
--- must_die
--- error_log
invalid parameter



=== TEST 6: a zone bound to two different keys is refused
--- main_config
stream {
    limit_req_zone $binary_remote_addr zone=t6:1m rate=1r/s;
    limit_req_zone $remote_addr zone=t6:1m rate=1r/s;
    server {
        listen 127.0.0.1:18596;
        limit_req zone=t6;
        return "x";
    }
}
--- config
    location = /t { return 200 ok; }
--- must_die
--- error_log
is already bound to key



=== TEST 7: a sound configuration starts and http is untouched
--- main_config
stream {
    limit_req_zone $binary_remote_addr zone=t7:1m rate=100r/s;
    limit_req_log_level info;
    limit_req_dry_run off;
    server {
        listen 127.0.0.1:18597;
        limit_req zone=t7 burst=20 nodelay;
        return "x";
    }
}
--- config
    location = /t { return 200 ok; }
--- request
GET /t
--- response_body chomp
ok
--- no_error_log
[alert]
