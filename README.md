# llt - low-latency trading engine

A market data and execution engine for Binance USDⓈ-M perpetual futures, written in
C++20 with no runtime dependencies beyond OpenSSL.

**Status: market data plane complete and measured. No orders are sent yet.**
See [docs/PLAN.md](docs/PLAN.md) for the roadmap and [docs/FINDINGS.md](docs/FINDINGS.md)
for what measurement has turned up so far.

---

## What this is, and what it is honest about

The goal is a market making bot. Before writing a strategy, the project answers a
cheaper question first: *can a quoted spread on this venue pay for its own fees at a
size a small account can afford?*

That question has a hard numeric form. At VIP0 the maker fee is 0.02% per side, so a
round trip costs **4 basis points**. BTCUSDT quotes a one-tick spread of **0.0124 bps**.
The spread is roughly 1/320th of the fee. No amount of latency engineering closes that
gap, so BTCUSDT is not a market making target, and the tooling says so out loud:

```
quoted spread (basis points)
  min 0.0124   p50 0.0124   p90 0.0124   max 0.0124
  maker round trip at VIP0 costs 4.0 bps; a spread below that cannot pay.
```

The same reasoning drives the rest of the design. Every phase ends in a measurement
that can veto the next one, so a bad answer arrives early and cheaply rather than after
the strategy is written.

## Architecture

```
            ┌─────────────────────────────────────────┐
            │  venue adapter  (Binance today)         │
            │  ┌───────────┐   ┌──────────────────┐   │
   wire ───▶│  │ WsClient  │──▶│ dispatch_payload │   │──▶ MdHandler
            │  └───────────┘   └──────────────────┘   │    (concept)
            │        │                  ▲             │
            └────────┼──────────────────┼─────────────┘
                     │                  │
              RecordWriter        RecordReader
                     │                  │
                  .mdlog ───────────────┘
```

Three properties are deliberate:

**Replay runs the live code.** `dispatch_payload` is a free function shared by the live
feed and the replayer. A backtest therefore exercises the program that will trade, not a
parallel reimplementation of it.

**Recordings store raw frames, not decoded events.** A capture stays valid when the
parser changes, and doubles as a regression corpus for the parser itself.

**Feeds are templates on their handler, not virtual interfaces.** Dispatch costs
nothing and the compiler inlines the consumer into the decode loop.

### Layout

| Path | Contents |
| --- | --- |
| `include/llt/core/` | fixed-point arithmetic, TSC clock, lock-free SPSC ring, CPU pinning |
| `include/llt/net/` | TCP, TLS, WebSocket (RFC 6455), HTTP/1.1 keep-alive |
| `include/llt/venue/` | venue-neutral event types, JSON scanners, `MdHandler` concept |
| `include/llt/venue/binance/` | Binance decode, market data feed, REST client |
| `include/llt/md/` | recording format, writer and reader |
| `src/tools/` | diagnostic and research tools |
| `tests/` | unit tests on a dependency-free harness |

## Design notes

**Prices are never floating point.** Every price and size is an `int64` scaled by 1e8.
`0.1` is not representable in a double, and a price off by one ULP is a rejected order
or a silently wrong tick comparison. Tick rounding uses floor division, because
truncating division rounds negative values the wrong way and breaks alignment below zero.

**JSON is scanned in one pass.** Searching for `"key":` per field rescans the buffer
once per field - nine passes over a `bookTicker` payload. `ObjectScanner` visits each
byte once and dispatches on the first character of the key.

**Certificate and hostname verification are not configurable.** This connection will
carry signed order requests. `SSL_set1_host` is the load-bearing call: without it a
valid certificate for *any* domain would be accepted.

## Build

Requires CMake ≥ 3.20, a C++20 compiler, and OpenSSL development headers.

```bash
./build.sh release          # also runs the unit tests
./build.sh debug
./build.sh asan             # or tsan, ubsan
./build.sh release --clean
```

All profiles compile clean under
`-Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast`.

CI covers gcc and clang across Debug and Release, plus address, thread and undefined
sanitizers.

## Tools

| Tool | Purpose |
| --- | --- |
| `llt_bot` | core micro-benchmarks and environment smoke test |
| `lat_probe` | NTP-style round-trip and clock-offset measurement |
| `md_dump` | live feed inspection; `LLT_RAW=1` dumps undecoded frames, `LLT_SUB` sends a subscription |
| `sym_scan` | two-stage ranking of every perpetual by spread, affordability and activity |
| `md_record` | long-running capture with reconnect and file rotation |
| `md_replay` | replays a capture through the live decode path |

```bash
./build/release/lat_probe 30
./build/release/sym_scan --secs 30 --deep-secs 60 --budget 20 --csv out.csv
./build/release/md_record --symbols TACUSDT,CFGUSDT --out data/md --rotate-mins 60
./build/release/md_replay data/md-*.mdlog
```

## Measured performance

Core primitives, Release build, AMD Ryzen 7 7840HS under WSL2. WSL2 virtualises `rdtsc`,
so the resolution is quantised to roughly 10 ns; treat these as upper bounds.

```
rdtsc_serialized pair    p50    20 ns   p99    30 ns
parse_fixed              p50    30 ns   p99    30 ns     (~10 ns net of timer overhead)
spsc handoff (1 hop)     p50    70 ns   p99   175 ns
spsc throughput          256 M msg/s
```

The SPSC figure is a ping-pong round trip over two rings with exactly one message in
flight, halved. Measuring push-to-pop on a single ring with a free-running producer
reports queue residency instead and gives numbers three orders of magnitude worse.

Network, measured against `fapi.binance.com` with a kept-alive connection:

```
from Belgrade            rtt  p50 261.3   p90 307.5   p99 318.0   ms
from AWS ap-northeast-1  rtt  p50   3.65  p90   4.91  p99   5.92  ms
```

The jitter matters more than the latency: p99 minus p50 falls from 57 ms to 2.3 ms.
Unpredictable delay cannot be compensated for in a quoting strategy; a constant one can.

## Security

- API keys are read from the environment only, never from a file in the repository.
- `.gitignore` covers `*.env`, `config/*.local.toml`, `secrets/` and `data/`.
- The planned production key is Ed25519, restricted to one IP, with withdrawals disabled.
- No credential has ever been committed, and none is required to run any tool here:
  every endpoint used so far is public and read-only.

## License

Not yet chosen.
