# Findings

Measurements that changed a decision. Each entry records how it was measured, what came
back, and what it implies - including the ones that were initially measured wrong.

All data collected 2026-10-27 from Belgrade against Binance USDⓈ-M futures, using only
public read-only endpoints.

---

## 1. BTCUSDT cannot pay its own fees

**Method.** Live `bookTicker`, spread expressed in basis points of the mid.

```
bid 80944.60   ask 80944.70   ->  spread 0.10 = one tick

min 0.0124   p50 0.0124   p90 0.0124   max 0.0124  (bps)
```

**Implication.** A VIP0 maker round trip costs 4 bps. The quoted spread is 0.0124 bps,
about 1/320th of the fee. There is no latency improvement that closes a 320× gap.

Affordability rules it out independently: BTCUSDT has `minQty` 0.001, so at ~$80,900 the
smallest order is $80.80 of notional. A two-sided quote needs $161 - eight times the
available capital before leverage.

This is the reference case the whole project is calibrated against.

## 2. Four documented market data streams deliver nothing

**Method.** Subscribed one stream at a time and dumped undecoded frames, counting every
event type including binary frames and idle polls.

| Stream | Result |
| --- | --- |
| `btcusdt@bookTicker` | works |
| `btcusdt@depth20@100ms` | works |
| `btcusdt@trade` | works |
| `btcusdt@aggTrade` | **silent** |
| `btcusdt@markPrice@1s` | **silent** |
| `btcusdt@kline_1m` | **silent** |
| `btcusdt@forceOrder` | silent (expected; liquidations are rare) |

Twenty seconds on `@aggTrade` produced 525,590 idle polls, zero frames, zero binary
frames, no ping, no close. Meanwhile REST `/fapi/v1/aggTrades` confirmed BTCUSDT was
printing three trades within 400 ms, so the market was active.

Both subscription mechanisms behave identically. Using the live method, the venue
*acknowledges* the subscription and then sends nothing:

```
-> {"method":"SUBSCRIBE","params":["btcusdt@aggTrade"],"id":1}
<- {"result":null,"id":1}
   (nothing further, 20 s)
```

**Implication.** The official documentation still lists `@aggTrade` as valid for
USDⓈ-M futures and does **not** list `@trade` for futures at all - only for spot. The
documentation and the running system disagree.

Trade flow now uses `@trade`, which is the better input anyway: prints are individual
rather than merged, so trade-flow imbalance is finer-grained. The decoder accepts both
shapes, which differ only in the id field (`t` versus `a`).

Mark price is still needed for risk checks in F6; it will come from REST
`/fapi/v1/premiumIndex` instead.

**How it was isolated.** The failure looked at first like a decoding bug. Ruling that
out took three steps, each cheap: dump raw frames (proves nothing arrives rather than
nothing parses), count events by type (rules out silently dropped binary frames), then
switch subscription mechanism (rules out the URL form). The diagnostic switches
`LLT_RAW` and `LLT_SUB` were kept in `md_dump` for the next time.

## 3. The all-market stream is throttled, and reports a fictional update rate

**Method.** Subscribed `!bookTicker`, counted updates per symbol over 45 s.

Every one of 523 symbols reported **exactly 0.200 updates per second** - one message per
symbol per five seconds, about 80 messages per second across the entire market.

```
BTCUSDT,0.012375,...,0.200,...
ETHUSDT,0.042615,...,0.200,...
SOLUSDT,0.988289,...,0.200,...
DOGEUSDT,1.272669,...,0.200,...
```

BTCUSDT alone produces roughly 240 updates per second on its dedicated stream.

**Implication.** The number measured the stream's cadence, not the market's activity.
A uniform value across every symbol was the tell: real markets are not uniform.

The spread values themselves are correct - BTCUSDT's 0.0124 bps matches the dedicated
stream exactly - but they come from only about nine samples per symbol.

This forced `sym_scan` into two stages: `!bookTicker` for a cheap wide sweep on spread
and affordability, dedicated streams for the shortlist where activity actually matters.

## 4. Activity measurements are far noisier than one sample suggests

**Method.** The same symbols measured twice, minutes apart, by two different tools.

| Symbol | `sym_scan`, 60 s | `md_replay`, 43 s |
| --- | --- | --- |
| TACUSDT | 51 trades/min | **11 trades/min** |
| CFGUSDT | 42 trades/min | **79 trades/min** |
| BTCUSDT | - | 1459 trades/min |

**Implication.** A fivefold swing on TACUSDT between two samples a few minutes apart.
One minute of observation does not characterise an illiquid symbol, so the F2 ranking is
a hypothesis rather than a conclusion.

This is the direct justification for F3.5: two weeks of capture across all trading
sessions, rather than trusting a single measurement.

## 5. Round-trip time from Belgrade, and a clock that cannot be trusted

**Method.** Thirty NTP-style samples against `/fapi/v1/time` over one keep-alive
connection.

```
rtt     min 258.1   p50 261.3   p90 307.5   p99 318.0   max 455.7   ms
offset  min 692.0   p50 789.1   p90 790.6   p99 791.4   max 791.5   ms
```

**Implication on latency.** One way is roughly 131 ms. A resting quote cannot be pulled
in under a quarter of a second, while a colocated competitor needs about one. From here
a market maker is a free option for everyone faster, which is every serious participant.
Execution has to move to AWS `ap-northeast-1`.

The 57 ms spread between p50 and p99 is consumer-internet jitter, and for quoting that
is worse than the latency itself: unpredictable is harder to compensate than slow.

**Implication on the clock.** The local clock sits 789 ms behind the venue - and it
*drifts*: an earlier measurement showed 622 ms. Roughly 170 ms of drift in twenty
minutes, because WSL2 has no real-time clock and takes its time from the host.

A constant correction would therefore not work. This is why latency measurement was
built NTP-style from the start: it derives both the round trip and the offset from the
same exchange, and needs no disciplined local clock. The same property will matter on a
fresh cloud instance before `chrony` has settled.

## 5a. The same measurement from Tokyo

**Method.** Identical tool, identical endpoint, run from a `t3.small` in AWS
`ap-northeast-1` with `chrony` running.

```
rtt     min 3.192   p50 3.653   p90 4.911   p99 5.915   max 9.122   ms
offset  min -0.746  p50 0.065   p90 0.514   p99 1.359   max 2.815   ms
```

|  | Belgrade | Tokyo | ratio |
| --- | ---: | ---: | ---: |
| rtt p50 | 261.3 ms | 3.65 ms | 72× |
| rtt p99 | 318.0 ms | 5.92 ms | 54× |
| jitter (p99 − p50) | 56.7 ms | 2.26 ms | 25× |
| clock offset | 789 ms, drifting | 0.065 ms | - |

**Implication.** The 25× reduction in jitter is the more valuable half. A constant delay
can be modelled and priced into a quote; a delay that varies by 57 ms cannot, because
the strategy never knows how stale its own view is.

The clock result is a side effect worth noting: with `chrony` the offset is 65
microseconds, so exchange timestamps on this host are directly comparable to local ones.
The NTP-style measurement is still what the engine uses, because it must also be correct
on a freshly booted instance before time sync has converged.

A naive reading of a single `curl` on the same box suggested 42 ms per request. That
figure is a fresh TLS handshake each time - about 31 ms of it CPU-bound cryptography on
a burstable instance, not network. It is the reason the HTTP client holds connections
open.

## 6. The first SPSC benchmark was wrong by three orders of magnitude

**Method, originally.** Producer pushes timestamps into a ring as fast as it can;
consumer pops and records the delta.

```
spsc handoff (1 hop)   p50 64861 ns   p99 108322 ns
```

**What was actually measured.** The producer outruns the consumer, the ring fills, and
every message waits behind roughly 1024 others. That is queue residency, not handoff
latency.

**Corrected method.** Ping-pong over two rings with exactly one message in flight,
halved.

```
spsc handoff (1 hop)   p50 70 ns   p99 175 ns
spsc throughput        256 M msg/s
```

**Implication.** Latency and throughput are separate questions and need separate
experiments. A benchmark that conflates them produces numbers that look catastrophic for
no reason - and the failure is silent, because the code is correct and only the
interpretation is wrong.

## 7. Where the candidates actually stand

Two-stage scan, 20 s wide and 60 s deep, $20 budget at 5× leverage, 4 bps fee hurdle,
$1M minimum 24h volume.

```
505 symbols -> 504 affordable -> 218 beat fees -> 151 liquid
22 of 25 shortlisted survived the deeper measurement
```

| symbol | spread bps | p90 bps | edge bps | trades/min |
| --- | ---: | ---: | ---: | ---: |
| DATAIPUSDT | 14.194 | 14.205 | 10.194 | 23 |
| BLURUSDT | 13.671 | 13.671 | 9.671 | 7 |
| CHRUSDT | 11.299 | 11.299 | 7.299 | 2 |
| CTKUSDT | 10.747 | 10.758 | 6.747 | 14 |
| OPENUSDT | 10.724 | 10.724 | 6.724 | 1 |
| CFGUSDT | 9.021 | 18.018 | 5.021 | 42 |
| CVXUSDT | 8.722 | 8.726 | 4.722 | 24 |
| TACUSDT | 8.442 | 16.878 | 4.442 | 51 |
| HANAUSDT | 6.838 | 13.699 | 2.838 | 117 |

Three readings matter more than the ranking:

**`trades/min` is the ceiling on fills.** A maker only earns when filled, and captures
some fraction of market flow. CHRUSDT's 7.3 bps edge is worth little at two prints per
minute; TACUSDT's 4.4 bps at 51 prints is worth considerably more.

**p90 far above the median means an unstable book.** DYDXUSDT runs 9.18 → 18.35 and
FHEUSDT 10.71 → 16.07. A spread that doubles is a book that gaps when something happens
- risk, not opportunity. CVXUSDT at 8.722 → 8.726 is the stable kind.

**DATAIPUSDT is suspiciously stable:** median 14.194, p90 14.205. A spread that does not
move usually means a dominant maker holding a fixed quote. Worth understanding why they
can afford to before standing opposite them.

And the caveat that applies to all of it: these are small-cap instruments. A wide spread
is payment for inventory risk, not free money. One 20% move erases hundreds of round
trips.

## 8. The strategy earns a positive gross edge and loses it to the fee

Five candidates replayed through the quoter and fill simulator over 2.37 hours of Tokyo
capture, at 4 bps round-trip fees.

| symbol | fills | capture bps/fill | net USDT | adverse 1s bps | blocked by queue |
| --- | ---: | ---: | ---: | ---: | ---: |
| CVXUSDT | 107 | +1.45 | -0.028 | -1.64 | 95.0% |
| DATAIPUSDT | 79 | +1.27 | -0.029 | -0.18 | 94.4% |
| TACUSDT | 99 | +0.93 | -0.052 | +0.21 | 94.9% |
| HANAUSDT | 146 | -0.34 | -0.158 | -2.92 | 93.6% |
| CFGUSDT | 50 | -0.60 | -0.062 | -0.48 | 97.4% |

Capture per fill is the figure to read, not the PnL: cents depend on order size, basis
points do not. Three of five produce a positive gross edge, none clears the 2.0 bps
maker fee. The loss is not to the market, it is to the exchange.

The right-hand column is the dominant fact. At minimum order size we rest 12 to 145
times our own size behind the front of the queue, so 93-97% of the trades that reach
our price never touch us. The ones that do are large enough to clear everything ahead -
which is to say, informed.

## 9. Quoting more patiently made it worse

Requoting roughly once a second against 85 fills means surrendering queue position some
129 times per fill, so a reluctance threshold should have helped. CVXUSDT, with quote
width and volatility held fixed:

| min requote ticks | requotes | capture bps | adverse 1s | blocked |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 12 244 | -0.59 | -1.57 | 93.2% |
| 8 | 5 502 | -1.92 | -3.08 | 93.9% |
| 64 | 5 585 | -2.43 | -2.62 | 93.7% |

Halving the requote count made capture worse and roughly doubled adverse selection,
while queue blocking did not move at all. A quote that stays put does not hold its place
so much as wait to be run over. The threshold also saturates: past 8 ticks the requote
count stops falling, because `should_replace` always fires when a side toggles on or
off, and by then the guards are doing most of the toggling.

A 96-cell sweep over width, volatility multiplier and requote reluctance returned three
profitable cells - with 14, 18 and 32 fills. Every cell with more than a hundred fills
lost. With 96 tries on one afternoon of data, selecting the best cell would have been
selecting noise.

## 10. The conclusion was an artefact of the fill model

`sim.hpp` drained the queue ahead of us only through trades. Real queues also drain
through cancellations, which this feed cannot observe. The bias was documented from the
start, and its direction was known; what was unknown was its size. So the cancellation
rate became a parameter and was swept rather than assumed.

TACUSDT, quote width held at 2 bps:

| decay /s | fills | capture bps | net USDT | adverse 1s | blocked |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0.0 | 76 | +1.16 | -0.032 | +0.66 | 95.5% |
| 0.5 | 294 | +4.40 | **+0.301** | +3.78 | 82.9% |
| 2.0 | 308 | +5.20 | **+0.446** | +4.22 | 81.7% |
| 8.0 | 329 | +5.21 | **+0.487** | +4.17 | 80.8% |
| 32.0 | 339 | +4.98 | **+0.469** | +4.19 | 80.1% |

The answer inverts at a decay of 0.5 per second - about 39% of the queue ahead leaving
each second, which for crypto perpetuals is a conservative rate rather than an extreme
one. DATAIPUSDT behaves the same way; CVXUSDT only at wider quotes.

Two details argue that this is a real effect and not a fitted one. The result is flat
from 0.5 through 32, so it does not balance on a particular value. And adverse selection
flips from neutral to clearly favourable, which is the predicted signature: the extra
fills arrive from benign flow, while the fills the pessimistic model allowed were the
informed ones that cleared the whole queue.

The honest conclusion is neither that this works nor that it does not. It is that the
answer is decided by a number nobody has measured yet, and that the earlier verdict -
reached with that number silently set to zero - was not evidence.
