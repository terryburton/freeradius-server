# FreeRADIUS EAP reproducers

One directory per issue. Each runs **eapol_test** (from hostap, a
submodule here) against an EAP server you name, checks one thing the
RFCs or FreeRADIUS's own documentation require, and prints `PASS` or
`FAIL` with the reason.

```sh
git submodule update --init          # hostap, pinned to hostap_2_10
mkdir certs && cp /etc/raddb/certs/{ca.pem,client.crt,client.key} certs/   # the server's test certificates
./01-eap-tls-framed-mtu/run <host> [port] [secret]     # default port 1812, secret testing123
```

Each `run` builds the eapol_test it needs, once, in `.build/`. A
reproducer that needs a misbehaving peer has its change to eapol_test in
its own `patch/` directory. Each patch is short and says what it does,
and applies only to that reproducer's build.

Exit status: 0 PASS, 1 FAIL, 2 could not run (a SKIP, with the reason).

Settings, from the environment:

| Variable | Default | For |
|---|---|---|
| `CERTS` | `./certs` | the directory with `ca.pem`, `client.crt`, `client.key` |
| `KEY_PASSWORD` | `whatever` | the client key's password (as in FreeRADIUS's raddb/certs) |
| `IDENTITY` | `user@example.org` | EAP-TLS identity (the client certificate's) |
| `ANON_IDENTITY` | `anonymous` | the outer identity of PEAP, TTLS and TEAP |
| `INNER_IDENTITY`, `PASSWORD` | `bob`, `bob` | the user inside a tunnel, and for EAP-pwd |
| `CA_KEY`, `CA_PASSWORD` | `$CERTS/ca.key`, `whatever` | to sign the large certificate of 04 and 05 |
| `MTU` | `600` | the NAS Framed-MTU in 01 and 02 |
| `PWD_FRAGMENT_SIZE` | (required by 03) | the server's EAP-pwd `fragment_size` |

The configurations (`eapol_test.conf`) are plain eapol_test network
blocks, with `@NAME@` placeholders for the settings above.

## The reproducers

| Dir | What it checks | Source | FreeRADIUS fixes |
|---|---|---|---|
| 01 | EAP-TLS behind a NAS with Framed-MTU 600: no EAP-Request over 596 octets | RFC 3579 2.4, RFC 3580 3.10 | v3.2.x #20, master #23 |
| 02 | PEAP with EAP-TLS inside, Framed-MTU 600: authenticates, no EAP-Request over 596 | as 01; the inner EAP Identifier must stay right over several rounds | v3.2.x #20 #28, master #23 #30 |
| 03 | EAP-pwd: packets no larger than `fragment_size` less 4, nor Framed-MTU less 4 just below it | raddb: pwd's fragment_size "has the same meaning as for TLS"; RFC 3579 2.4 | v3.2.x #24 #44, master #23 #45 |
| 04 | EAP-TLS with a client certificate of over 20K (a TLS message longer than one record): authenticates | RFC 5216 2.1.5 (TLS Message Length is the whole message) | v3.2.x #22, master #46 |
| 05 | More rounds than the server allows: an Access-Reject with an EAP-Failure, sent the first time | RFC 3579 2.6.2 | master #31 |
| 06 | The peer ACKs the EAP-TLS Start: not answered with an empty Request | RFC 5216 2.1.5 (the only empty Requests are the Start and a fragment ACK) | v3.2.x #43, master #37 |
| 07 | The peer ACKs the server's first flight instead of sending its own: not answered with an empty Request | as 06 | master #37 (v3.2.x already rejects) |
| 08 | PEAP: the peer ACKs instead of sending its data in the tunnel: not answered with an empty Request, never accepted | as 06 | v3.2.x #43, master #37 |
| 09 | One Response carries the previous Request's Identifier: not acted on | RFC 3748 4.1 | master #42 (v3.2.x already rejects) |
| 10 | TTLS-EAP-TLS with an AES-CBC-SHA384 outer session takes no more rounds than with AES-GCM | the server's own nesting allowance (inner fragments must fit one outer fragment) | v3.2.x #39, master #41 |
| 11 | TEAP: the peer ACKs instead of sending its Crypto-Binding: never accepted | RFC 7170 | (none needed: a regression check) |

The `#` numbers are draft pull requests on
[terryburton/freeradius-server](https://github.com/terryburton/freeradius-server/pulls),
tracked in issue #38.

## Results so far

Against FreeRADIUS's own test servers (`src/tests` on v3.2.x, each
`test.<name>` server on master):

| Dir | v3.2.x upstream (+ #21 to build) | v3.2.x with all the fixes |
|---|---|---|
| 01 | FAIL: 606 octets | PASS |
| 02 | FAIL: 610 octets | PASS |
| 03 (fragment_size 100) | FAIL: 102 octets | PASS |
| 04 | FAIL: rejected | PASS |
| 05 | FAIL\*: no EAP-Failure | SKIP: the test server allows 100 rounds, which is eapol_test's own limit |
| 06 | FAIL: empty Request | PASS |
| 07 | PASS | PASS |
| 08 | FAIL: empty Request | PASS |
| 09 | PASS | PASS |
| 10 | FAIL: 18 rounds against 15 | PASS |
| 11 | PASS | PASS |

\* v3.2.x's `src/tests` server has no `Post-Auth-Type REJECT { eap }`,
which the stock raddb has, and that is what adds the EAP-Failure.

## Notes

* eapol_test stops at 100 rounds itself, so 05 needs a server whose
  limit is lower. The default `max_rounds` is 50.
* 10 needs the server to allow both ciphers (with TLS 1.2), and, on
  master, TTLS's default `include_length = yes`.
* 11 needs TEAP with password authentication inside, as FreeRADIUS
  v3.2.x's `eap-teap-password` test has.
* Nesting a tunnel inside a tunnel (fixed for v3.2.x in #40) has no
  reproducer: eapol_test cannot be configured to do it.

These reproducers, and the patches, were written with an AI assistant
(Claude Code), and each has been run against an unfixed and a fixed
server as shown above.
