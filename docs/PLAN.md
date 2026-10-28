# Plan

Every phase ends in a measurement that can veto the next one. The point is that bad
news arrives in week two rather than month six, and that nothing built before the
strategy is chosen is wasted if the strategy changes.

## Constraints this plan is built around

| Constraint | Consequence |
| --- | --- |
| Developer in Belgrade, ~261 ms RTT to the venue | Execution must run in AWS `ap-northeast-1`; development stays local |
| $20 of risk capital | Live trading is a correctness test, not a profit attempt |
| VIP0 fees: 0.02% maker per side | Any symbol quoting under 4 bps is a guaranteed loss |
| No market maker agreement | Competitors operate at zero or negative maker fees |

The capital figure is the one that reframes everything. At $20, an excellent 50% annual
return is $10. The project's expected value is in the engineering and the portfolio, not
the PnL - so the system is built as though it were real, and trading profit is treated
as the signal that it works rather than the goal.

## Phases

| Phase | Work | Exit question | Status |
| --- | --- | --- | --- |
| **F0** | Build, core primitives, CI | - | done |
| **F1** | TCP/TLS/WebSocket, venue adapter, decode | - | done |
| **F2** | `lat_probe`, `sym_scan`, `md_record`, `md_replay` | Does any symbol beat the fee hurdle? | done - conditional pass |
| **F3** | Deploy to AWS Tokyo | What is the real round-trip time? | done - 3.65 ms p50 |
| **F3.5** | Continuous capture from Tokyo | Which strategy, decided from data? | running |
| **F4** | Signals, quoter, backtest over replay | Positive expectancy in replay? | built, awaiting data |
| **F5** | Ed25519 WebSocket API, order manager | - | in progress |
| **F6** | Risk engine, dead-man switch, chaos tests | - | limits built, chaos tests pending |
| **F7** | Live smoke test, $20, one symbol | Does live agree with replay? | pending |
| **F8** | Scale | Only if F4 and F7 agree | pending |

---

## F0 - Foundations · done

Repository arrived with a broken build: `CMakeLists.txt` referenced `src/main.cpp`
while the file on disk was an empty `src/main.c`.

- CMake restructured into `llt_flags` (warning policy), `llt_core`, executables and tests.
  Options for native arch, tests and sanitizer selection; LTO in Release only, since it
  breaks sanitizer symbolisation.
- `build.sh` gained profiles (`release`/`debug`/`asan`/`tsan`/`ubsan`) and stopped
  launching the binary automatically - acceptable for a demo, not for something that
  will eventually place orders.
- Core primitives: `fixed`, `clock`, `spsc_ring`, `cpu`.
- Dependency-free test harness, GitHub Actions CI, `.clang-format`, `.gitignore`.

`log.hpp`, `config.hpp` and `result.hpp` were deliberately deferred: they had no
consumer yet, and an abstraction written blind is worse than no abstraction.

**Verification.** All five build profiles clean under `-Werror`; tests pass under
address, thread and undefined sanitizers.

## F1 - Market data plane · done

### F1a - Network

`TcpSocket` (non-blocking connect with timeout, `TCP_NODELAY`), `TlsStream` (OpenSSL 3,
mandatory chain and hostname verification), `WsClient` (RFC 6455 client: handshake with
`Sec-WebSocket-Accept` validation, client-side masking, fragment reassembly, automatic
pong), `HttpClient` (HTTP/1.1 with keep-alive, because a fresh connection costs four
round trips).

`permessage-deflate` is deliberately unsupported: decompression adds a variable-latency
step to the hot path for payloads of a few hundred bytes.

### F1b - Decode

Venue-neutral event types (`BookTicker`, `DepthSnapshot`, `Trade`), single-pass JSON
scanners, Binance decode, REST client for `exchangeInfo` and `ticker/24hr`.

**Deviation from the original plan.** The plan called for the full `@depth` diff stream
with `U`/`u`/`pu` sequence tracking and REST resynchronisation. Partial depth snapshots
(`@depth20@100ms`) were used instead: each message is self-contained, so there is no
sequence state, no gap handling, and no failure mode where the local book silently
diverges from the venue. Order book imbalance over the top levels - the signal a market
maker actually needs - comes directly from the snapshot. The sequence fields are still
parsed and stored, so a full book can be built later if something requires it.

## F2 - Measurement · done, conditional pass

Three tools, and three findings that changed the plan. The findings are documented in
[FINDINGS.md](FINDINGS.md); this section records what was built and decided.

### `lat_probe`

NTP-style measurement against `/fapi/v1/time`:

$$\text{rtt} = t_1 - t_0, \qquad \text{offset} = t_{\text{server}} - \frac{t_0 + t_1}{2}$$

Neither figure depends on the local clock being correct - which matters, because the
development machine's clock drifts by roughly 170 ms every twenty minutes, and a fresh
cloud instance will not have run `chrony` yet.

### `sym_scan`

Two stages, because the all-market stream misreports activity:

1. `!bookTicker` covers every symbol at once but is throttled by the venue to one update
   per symbol per five seconds. Usable for estimating a spread and rejecting the
   unaffordable; useless for measuring how busy a symbol is.
2. Dedicated `@bookTicker` and `@trade` streams for the survivors give the real update
   rate, spread distribution and print frequency.

Affordability accounts for both floors an order must clear:
`min_notional = max(MIN_NOTIONAL, minQty × price)`, with margin at the configured
leverage.

### `md_record` / `md_replay`

Raw frames to disk with reconnect and rotation; replay through `dispatch_payload`, the
same function the live feed uses. Determinism is verified by hashing two replay runs of
the same capture.

### Outcome

```
505 symbols -> 504 affordable -> 218 beat fees -> 151 liquid
22 of 25 survived the deeper measurement
```

A conditional pass. Candidates exist, but the symbols with the widest spreads have
almost no trade flow, and `trades/min` is the ceiling on fills no matter how wide the
spread is. The ranking is a hypothesis, not a conclusion - see the measurement-noise
finding.

---

## F3 - Deployment to Tokyo · done

AWS `ap-northeast-1`, `t3.small` on x86 (not ARM, so `__rdtsc` and `-march=native`
remain available), Elastic IP so the eventual API key can be pinned to it, `chrony`,
`systemd` with `Restart=always`, secrets via `EnvironmentFile` with mode 600, and a
`deploy.sh` that builds, rsyncs and restarts.

**Known gotcha.** `t3` instances are burstable. A busy-poll loop pins a core at 100% and
exhausts CPU credits within minutes, after which the instance is throttled to its
baseline. The engine therefore starts on `epoll`, with busy-poll behind a configuration
flag. Busy-poll saves single-digit microseconds; TLS, WebSocket framing and JSON
decoding cost hundreds. It is not the bottleneck.

**Storage.** The projection of 2.2 GB/day was pessimistic: it came from a sample where
BTCUSDT alone was 84% of the volume. The five candidate symbols actually run at about
850 MB/day uncompressed, roughly 100 MB/day after the hourly gzip, so a fortnight fits
in a couple of gigabytes.

**Result.** Round trip from Tokyo is p50 3.65 ms against 261 ms from Belgrade, and
jitter falls from 57 ms to 2.3 ms. The latter is the more valuable half: constant delay
can be priced into a quote, variable delay cannot. Details in
[FINDINGS.md](FINDINGS.md#5a-the-same-measurement-from-tokyo).

**Endpoint note.** `testnet.binancefuture.com` now redirects to a demo environment tied
to the real account. `bn_env.hpp` holds the full map so demo and production hostnames
cannot be mixed; market data recording deliberately defaults to production, since
reading the real book is harmless and is the point of the exercise.

## F3.5 - Continuous capture · running

Two weeks of recording from Tokyo across Asian, European and US sessions. This runs as a
`systemd` service and does not block development: F4 and F5 are built while it fills.

Candidates: the symbols surviving F2, plus BTCUSDT as a control whose economics are
known to be negative.

## F4 - Strategy · built, awaiting data

Microprice, order book imbalance, trade-flow imbalance, realised volatility. Reservation
price skewed by inventory, half-spread bounded below by the fee break-even. Requote
policy with a tick threshold and a minimum interval, since requoting on every tick burns
rate limit and destroys queue position. Adverse-selection guards that pull quotes on
extreme imbalance or one-directional flow.

Signals are driven by matching-engine time rather than local arrival time, so a replay
produces identical numbers on any host at any speed. Flow signals stay neutral until
enough trades have accumulated: an average seeded with its first observation reports a
single print as a total imbalance, which would have the quoter react to one trade.

The fill simulator models queue position explicitly and states its biases in
`sim.hpp`. The first run, over a 43-second sample, produced no fills and explained why:
the required spread put the quote at the touch behind 1650 units of resting size, with
an order of 45. Queue position, not spread, was the binding constraint. Whether that
holds over real data is the question F3.5 exists to answer.

## F5 - Execution · in progress

Done: Ed25519 signing validated against RFC 8032 test vectors, a key generator that
never prints a private key, and a WebSocket API client with `session.logon` so no
request carries a signature. `auth_check` proves a credential end to end over both REST
and the socket before anything is built on it.

Remaining: `order.place` with `timeInForce=GTX` (post-only: rejected rather than
crossing and paying taker fees), `order.modify`, batch operations. User data stream for
`ORDER_TRADE_UPDATE` and `ACCOUNT_UPDATE`. Order manager with a fixed slot pool and an
explicit state machine, reconciled against REST. Token-bucket rate limiting applied
before sending rather than after rejection.

## F6 - Risk · limits built, chaos tests pending

Built. The layer separates two things that are usually conflated. A *veto* refuses one
order and clears itself when the cause goes away. A *halt* latches and only an operator
clears it, because a limit that can un-trip itself will un-trip at the worst possible
moment - an unrealised bounce makes equity look survivable and trading resumes exactly
when it is least likely to be right.

Vetoes: position and notional ceilings measured against the position the order *would
create* rather than the one held, per-order notional, price sanity band, stale feed.
A stale feed also pulls resting orders, since those are priced off a book we can no
longer see. Halts: loss limit counted on equity including open positions, reject storm,
kill switch, feed gone past a minute.

A default-constructed `RiskConfig` permits nothing - zero means deny, not unlimited, so
a config nobody filled in stops the engine instead of waving it through.

The dead-man switch is the only protection that survives the engine dying; everything
else assumes we are alive to enforce it. `countdownCancelAll` holds a timer at the
venue that we refresh at a third of its length, so two consecutive missed refreshes are
needed before a healthy book is flattened. The kill switch is a file, so stopping
trading needs nothing but SSH and survives a restart.

Still pending: wiring the transport, and chaos tests - `kill -9`, severed network,
websocket disconnect, system clock jump.

## F7 - Live smoke test · pending

Testnet for 48 hours, then $20 live on a single symbol at minimum size.

At that size no strategy earns anything meaningful - a round trip on a $5 order at
10 bps is half a cent. That is the correct use of the money: it proves the order
lifecycle, fee accounting, reconciliation and dead-man switch against the real matching
engine, which testnet cannot, because testnet has different liquidity and forgiving
fills. Profitability is judged separately, from replay over recorded data.

## F8 - Scale · pending

Only when replay and live agree.

---

## Out of scope

Cross-exchange arbitrage, FPGA or kernel bypass (meaningless over a WAN), options, GUI,
multiple accounts, a separate Python backtesting stack.
