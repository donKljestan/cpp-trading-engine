// Records raw market data frames to disk for later replay.
//
// Built to survive a long unattended capture: the venue drops connections, so the
// recorder reconnects with backoff, and files rotate so one corrupted tail cannot
// cost a whole run.
//
//   md_record --symbols TACUSDT,CFGUSDT --out data/md [--rotate-mins 60] [--secs 0]

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "llt/core/clock.hpp"
#include "llt/md/record.hpp"
#include "llt/net/ws.hpp"
#include "llt/venue/binance/bn_feed.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Options {
    std::string symbols = "BTCUSDT";
    std::string out     = "md";
    // Research wants the real book. Reading it places no orders and risks nothing.
    std::string env         = "production";
    int         rotate_mins = 60;
    int         secs        = 0;  // 0 means run until interrupted
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const char* const      val  = argv[i + 1];
        if (flag == "--symbols") {
            o.symbols = val;
        } else if (flag == "--out") {
            o.out = val;
        } else if (flag == "--env") {
            o.env = val;
        } else if (flag == "--rotate-mins") {
            o.rotate_mins = static_cast<int>(std::strtol(val, nullptr, 10));
        } else if (flag == "--secs") {
            o.secs = static_cast<int>(std::strtol(val, nullptr, 10));
        }
    }
    return o;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t                   begin = 0;
    while (begin <= s.size()) {
        const size_t end = s.find(sep, begin);
        if (end == std::string::npos) {
            if (begin < s.size()) out.push_back(s.substr(begin));
            break;
        }
        if (end > begin) out.push_back(s.substr(begin, end - begin));
        begin = end + 1;
    }
    return out;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return out;
}

std::string timestamped_path(const std::string& prefix) {
    const std::time_t now = std::time(nullptr);
    std::tm           tm{};
    ::gmtime_r(&now, &tm);

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    return prefix + "-" + stamp + ".mdlog";
}

}  // namespace

int main(int argc, char** argv) {
    // Under systemd stdout is a pipe, so it is block buffered by default and
    // operational messages would sit unflushed for the life of the process.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    llt::clk::clock_init();
    const Options opt = parse_args(argc, argv);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::vector<std::string> streams;
    for (const auto& sym : split(opt.symbols, ',')) {
        const std::string s = to_lower(sym);
        streams.push_back(s + "@bookTicker");
        streams.push_back(s + "@depth20@100ms");
        streams.push_back(s + "@trade");
    }
    if (streams.empty()) {
        std::fprintf(stderr, "no symbols given\n");
        return 1;
    }

    const llt::binance::Endpoints& env = llt::binance::env_by_name(opt.env.c_str());

    std::printf("recording %zu streams from %s (%s) to %s-*.mdlog\n", streams.size(),
                env.md_ws, env.name, opt.out.c_str());
    std::printf("rotating every %d min; ctrl-c to stop\n\n", opt.rotate_mins);

    const uint64_t rotate_ns =
        static_cast<uint64_t>(opt.rotate_mins) * uint64_t{60'000'000'000};
    const uint64_t run_deadline =
        (opt.secs > 0)
            ? llt::clk::mono_ns() + static_cast<uint64_t>(opt.secs) * uint64_t{1'000'000'000}
            : UINT64_MAX;

    llt::md::RecordWriter writer;
    llt::net::WsClient    ws;

    uint64_t file_opened_ns = 0;
    uint64_t total_records  = 0;
    uint64_t total_bytes    = 0;
    uint64_t reconnects     = 0;
    int      backoff_s      = 1;

    // A gap in the capture cannot be recovered afterwards, so its length is the one
    // thing the log has to state plainly.
    uint64_t connected_ns = 0;
    uint64_t dropped_ns   = 0;
    uint64_t lost_ns      = 0;

    while (g_stop == 0 && llt::clk::mono_ns() < run_deadline) {
        if (!ws.valid()) {
            if (reconnects != 0) {
                std::printf("reconnecting in %ds ...\n", backoff_s);
                for (int i = 0; i < backoff_s * 10 && g_stop == 0; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (g_stop != 0) break;
            }

            if (!ws.connect(env.md_ws, 443, "/ws", 10'000) ||
                !ws.send_text(llt::binance::subscribe_message(streams, 1))) {
                std::fprintf(stderr, "connect failed: %s\n",
                             llt::net::WsClient::last_error());
                backoff_s = (backoff_s < 30) ? backoff_s * 2 : 30;
                ++reconnects;
                continue;
            }

            backoff_s = 1;
            ++reconnects;
            connected_ns = llt::clk::mono_ns();
            if (dropped_ns != 0) {
                const uint64_t gap = connected_ns - dropped_ns;
                lost_ns += gap;
                std::printf("connected (attempt %lu), gap %.1f s, %.1f s lost today\n",
                            static_cast<unsigned long>(reconnects),
                            static_cast<double>(gap) / 1e9,
                            static_cast<double>(lost_ns) / 1e9);
                dropped_ns = 0;
            } else {
                std::printf("connected (attempt %lu)\n",
                            static_cast<unsigned long>(reconnects));
            }
        }

        if (!writer.is_open() || llt::clk::mono_ns() - file_opened_ns > rotate_ns) {
            if (writer.is_open()) {
                total_records += writer.records();
                total_bytes += writer.bytes();
                writer.close();
            }
            const std::string path = timestamped_path(opt.out);
            if (!writer.open(path)) {
                std::fprintf(stderr, "cannot open %s for writing\n", path.c_str());
                return 1;
            }
            file_opened_ns = llt::clk::mono_ns();
            std::printf("writing %s\n", path.c_str());
        }

        std::string_view payload;
        const auto       ev = ws.poll(payload);

        if (ev == llt::net::WsClient::Event::Text) {
            if (!writer.write(ws.last_recv_tsc(), llt::clk::wall_ns(), payload)) {
                std::fprintf(stderr, "write failed, disk full?\n");
                return 1;
            }
        } else if (ev == llt::net::WsClient::Event::Close ||
                   ev == llt::net::WsClient::Event::Error) {
            // An empty reason means the venue closed the socket cleanly, which it does
            // to cycle long-lived connections. That is routine; an error is not.
            const char* why = llt::net::WsClient::last_error();
            const bool  clean = (ev == llt::net::WsClient::Event::Close);
            dropped_ns = llt::clk::mono_ns();

            std::fprintf(stderr, "stream dropped after %.0f s: %s%s\n",
                         connected_ns != 0
                             ? static_cast<double>(dropped_ns - connected_ns) / 1e9
                             : 0.0,
                         clean ? "closed by venue" : "error",
                         (why != nullptr && *why != '\0') ? why : "");
            ws.close();
        }
    }

    total_records += writer.records();
    total_bytes += writer.bytes();
    writer.close();
    ws.close();

    std::printf("\nstopped. %lu records, %.1f MB, %lu connection(s), %.1f s not captured\n",
                static_cast<unsigned long>(total_records),
                static_cast<double>(total_bytes) / 1e6,
                static_cast<unsigned long>(reconnects),
                static_cast<double>(lost_ns) / 1e9);
    return 0;
}
