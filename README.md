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
| `PWD_FRAGMENT_SIZE` | (required by 03) | the server's EAP-pwd `fragment_size`: 105 tests both of 03's checks |

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

Against FreeRADIUS's own test servers: `src/tests` on v3.2.x, and the
`test.<name>` server for each method on master (with that test's
identities, from the environment).

| Dir | v3.2.x upstream (+ #21 to build) | v3.2.x, all fixes | master upstream (+ #36) | master, all fixes |
|---|---|---|---|---|
| 01 | FAIL: 606 octets | PASS | FAIL: 1020 octets (Framed-MTU ignored) | PASS |
| 02 | FAIL: 610 octets | PASS | FAIL: 1020 octets | PASS |
| 03 | FAIL: 102 octets (fragment_size 100) | PASS | FAIL: 102 > 101 (fragment_size 105) | PASS |
| 04 | FAIL: rejected | PASS | FAIL: rejected | PASS |
| 05 | FAIL\*: no EAP-Failure | SKIP: allows 100 rounds | PASS | PASS |
| 06 | FAIL: empty Request | PASS | FAIL: empty Request | PASS |
| 07 | PASS | PASS | FAIL: empty Request (10 octets: L, length 0) | PASS |
| 08 | FAIL: empty Request | PASS | FAIL: PEAP starts phase 2 again | PASS |
| 09 | PASS | PASS | FAIL: acted on, and accepted | PASS |
| 10 | FAIL: 18 rounds against 15 | PASS | FAIL: 16 against 14 | PASS |
| 11 | PASS | PASS | (no TEAP) | (no TEAP) |

\* v3.2.x's `src/tests` server has no `Post-Auth-Type REJECT { eap }`,
which the stock raddb has, and that is what adds the EAP-Failure.

05 passes on upstream master: there, EAP's own limit of 50 rejects at
once. It is a regression check for #31, whose first version sent no
reply at `session.max_rounds`.

## Notes

* eapol_test stops at 100 rounds itself, so 05 needs a server whose
  limit is lower. The default `max_rounds` is 50.
* 10 needs the server to allow both ciphers (with TLS 1.2), and, on
  master, TTLS's default `include_length = yes` (master's
  `ttls-client-eap-tls` test sets `no`, which saves exactly the 4 octets
  that hide the problem).
* 02 and 10 also offer the client certificate on the outer tunnel, for
  servers (such as master's tests) that ask for one; otherwise it is not
  sent.
* 06 to 08 expect the server to reject a peer whose ACK acknowledges
  nothing. RFC 5216 doesn't say what the server must do; rejecting at
  once is FreeRADIUS's choice, as v3.2.x already does for an ACK in the
  middle of the handshake.
* 11 needs TEAP with password authentication inside, as FreeRADIUS
  v3.2.x's `eap-teap-password` test has.
* No reproducer here for these, because eapol_test can't produce them
  without large changes. eap-frag-tools' `peer.py` tests them:
  * nesting a tunnel inside a tunnel (v3.2.x #40);
  * a peer sending valid tunnel data in place of an ACK (v3.2.x #47);
  * the previous round's flags read from a length octet (v3.2.x #48;
    shows only in the server's debug log);
  * TTLS with TLS 1.3, where the peer sends its first AVPs with its
    Finished, and gets no session ticket (#52, and master). eapol_test
    sends its Finished alone.

These reproducers, and the patches, were written with an AI assistant
(Claude Code), and each has been run against an unfixed and a fixed
server as shown above.
