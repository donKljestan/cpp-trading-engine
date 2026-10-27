#include "llt/md/record.hpp"

#include <cstring>

#include "llt/core/clock.hpp"

namespace llt::md {
namespace {

constexpr size_t kIoBuffer  = 1 << 20;
constexpr size_t kMaxRecord = 16 << 20;  // a frame larger than this means a corrupt file

}  // namespace

RecordWriter::~RecordWriter() { close(); }

bool RecordWriter::open(const std::string& path) {
    close();

    file_ = std::fopen(path.c_str(), "wb");
    if (file_ == nullptr) return false;

    buffer_.resize(kIoBuffer);
    std::setvbuf(file_, buffer_.data(), _IOFBF, buffer_.size());

    FileHeader h;
    h.start_wall_ns = llt::clk::wall_ns();
    h.start_mono_ns = llt::clk::mono_ns();
    h.tsc_hz        = static_cast<uint64_t>(llt::clk::tsc_ghz() * 1e9);

    if (std::fwrite(&h, sizeof(h), 1, file_) != 1) {
        close();
        return false;
    }
    bytes_ = sizeof(h);
    return true;
}

void RecordWriter::close() noexcept {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    records_ = 0;
    bytes_   = 0;
}

bool RecordWriter::write(uint64_t recv_tsc, uint64_t recv_wall_ns,
                         std::string_view payload) {
    if (file_ == nullptr || payload.size() > kMaxRecord) return false;

    RecordHeader h;
    h.recv_tsc     = recv_tsc;
    h.recv_wall_ns = recv_wall_ns;
    h.length       = static_cast<uint32_t>(payload.size());
    h.kind         = static_cast<uint8_t>(RecordKind::WsText);

    if (std::fwrite(&h, sizeof(h), 1, file_) != 1) return false;
    if (!payload.empty() &&
        std::fwrite(payload.data(), 1, payload.size(), file_) != payload.size()) {
        return false;
    }

    ++records_;
    bytes_ += sizeof(h) + payload.size();
    return true;
}

RecordReader::~RecordReader() { close(); }

bool RecordReader::open(const std::string& path) {
    close();

    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) return false;

    if (std::fread(&header_, sizeof(header_), 1, file_) != 1 ||
        header_.magic != kMagic || header_.version != kVersion) {
        close();
        return false;
    }
    return true;
}

void RecordReader::close() noexcept {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    truncated_ = false;
}

bool RecordReader::next(RecordHeader& out, std::string_view& payload) {
    if (file_ == nullptr) return false;

    if (std::fread(&out, sizeof(out), 1, file_) != 1) return false;  // clean EOF

    if (out.length > kMaxRecord) {
        truncated_ = true;
        return false;
    }

    payload_.resize(out.length);
    if (out.length != 0 &&
        std::fread(payload_.data(), 1, out.length, file_) != out.length) {
        // A recorder killed mid-write leaves a partial tail; report it rather than
        // silently treating it as the end of the data.
        truncated_ = true;
        return false;
    }

    payload = std::string_view(payload_.data(), out.length);
    return true;
}

}  // namespace llt::md
