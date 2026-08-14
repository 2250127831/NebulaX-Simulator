#include "replay/itch_replay_source.h"

#include <fstream>

bool ItchReplaySource::open(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const auto sz = f.tellg();
    if (sz <= 0) return false;
    data_.resize(static_cast<size_t>(sz));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(data_.data()), sz);
    if (!f && !f.eof()) return false;
    pos_ = 0;
    frame_count_ = 0;
    return true;
}

bool ItchReplaySource::next(const uint8_t*& body, size_t& len, uint64_t& seq) {
    if (pos_ + 2 > data_.size()) return false;
    const size_t l = (static_cast<size_t>(data_[pos_]) << 8) | data_[pos_ + 1];
    if (pos_ + 2 + l > data_.size()) return false;   // 尾部截断帧：丢弃（数据不完整）
    body = data_.data() + pos_ + 2;
    len = l;
    seq = frame_count_ + 1;
    pos_ += 2 + l;
    ++frame_count_;
    return true;
}
