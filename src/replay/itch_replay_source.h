#pragma once

// ── 历史 ITCH 文件回放源（★ 本仿真新增）──
// 逐帧读取 2B big-endian 长度前缀的 ITCH 二进制文件：
//   [len: u16 BE][body: len 字节]，body[0] 为消息类型字符。
// 一次性读入内存（单机仿真），next() 返回下一帧的 body 指针（指向内部缓冲）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class ItchReplaySource {
public:
    bool open(const std::string& path);
    void close() { data_.clear(); pos_ = 0; frame_count_ = 0; }

    // 读下一帧。body 指向内部缓冲（下一次调用失效）。返回 false = 已到 EOF 或帧损坏。
    bool next(const uint8_t*& body, size_t& len, uint64_t& seq);

    size_t frame_count() const { return frame_count_; }   // 已读帧数
    size_t file_size() const { return data_.size(); }

private:
    std::vector<uint8_t> data_;
    size_t pos_ = 0;
    size_t frame_count_ = 0;
};
