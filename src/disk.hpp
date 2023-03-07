// Copyright 2018 Chia Network Inc

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SRC_CPP_DISK_HPP_
#define SRC_CPP_DISK_HPP_

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

using namespace std::chrono_literals; // for operator""min;

#include "chia_filesystem.hpp"

#include "./bits.hpp"
#include "./util.hpp"
#include "bitfield.hpp"

constexpr uint64_t write_cache = 1024 * 1024;
constexpr uint64_t read_ahead = 1024 * 1024;

struct Disk {
    virtual uint8_t const* Read(uint64_t begin, uint64_t length) = 0;
    virtual void Write(uint64_t begin, const uint8_t *memcache, uint64_t length) = 0;
    virtual void Truncate(uint64_t new_size) = 0;
    virtual std::string GetFileName() = 0;
    virtual void FreeMemory() = 0;
    virtual ~Disk() = default;
};

struct BufferedDisk : Disk
{
    BufferedDisk(std::vector<uint8_t>* disk, uint64_t file_size) : disk_(disk), file_size_(file_size) {}

    uint8_t const* Read(uint64_t begin, uint64_t length) override
    {
        assert(length < read_ahead);
        NeedReadCache();
        // all allocations need 7 bytes head-room, since
        // SliceInt64FromBytes() may overrun by 7 bytes
        if (read_buffer_start_ <= begin
            && read_buffer_start_ + read_buffer_size_ >= begin + length
            && read_buffer_start_ + read_ahead >= begin + length + 7)
        {
            // if the read is entirely inside the buffer, just return it
            return read_buffer_.get() + (begin - read_buffer_start_);
        }
        else if (begin >= read_buffer_start_ || begin == 0 || read_buffer_start_ == std::uint64_t(-1)) {

            // if the read is beyond the current buffer (i.e.
            // forward-sequential) move the buffer forward and read the next
            // buffer-capacity number of bytes.
            // this is also the case we enter the first time we perform a read,
            // where we haven't read anything into the buffer yet. Note that
            // begin == 0 won't reliably detect that case, sinec we may have
            // discarded the first entry and start at some low offset but still
            // greater than 0
            read_buffer_start_ = begin;
            uint64_t const amount_to_read = std::min(file_size_ - read_buffer_start_, read_ahead);
            std::memcpy(static_cast<uint8_t*>(read_buffer_.get()), disk_->data() + begin, amount_to_read);
            read_buffer_size_ = amount_to_read;
            return read_buffer_.get();
        }
        else {
            // ideally this won't happen
            std::cout << "Disk read position regressed. It's optimized for forward scans. Performance may suffer\n"
                << "   read-offset: " << begin
                << " read-length: " << length
                << " file-size: " << file_size_
                << " read-buffer: [" << read_buffer_start_ << ", " << read_buffer_size_ << "]"
                << " file: not a file"
                << '\n';
            // TODO: This needs to become non-static for multi-threading
            static uint8_t temp[128];
            // all allocations need 7 bytes head-room, since
            // SliceInt64FromBytes() may overrun by 7 bytes
            assert(length <= sizeof(temp) - 7);

            // if we're going backwards, don't wipe out the cache. We assume
            // forward sequential access
            std::memcpy(temp, disk_->data() + begin, length);
            return temp;
        }
    }

    void Write(uint64_t const begin, const uint8_t *memcache, uint64_t const length) override
    {
        NeedWriteCache();
        if (begin == write_buffer_start_ + write_buffer_size_) {
            if (write_buffer_size_ + length <= write_cache) {
                ::memcpy(write_buffer_.get() + write_buffer_size_, memcache, length);
                write_buffer_size_ += length;
                return;
            }
            FlushCache();
        }

        if (write_buffer_size_ == 0 && write_cache >= length) {
            write_buffer_start_ = begin;
            ::memcpy(write_buffer_.get() + write_buffer_size_, memcache, length);
            write_buffer_size_ = length;
            return;
        }

        std::memcpy(disk_->data() + begin, memcache, length);
    }

    void Truncate(uint64_t const new_size) override
    {
        FlushCache();
        disk_->resize(new_size);
        file_size_ = new_size;
        FreeMemory();
    }

    std::string GetFileName() override { return "not a file"; }

    void FreeMemory() override
    {
        FlushCache();

        read_buffer_.reset();
        write_buffer_.reset();
        read_buffer_size_ = 0;
        write_buffer_size_ = 0;
    }

    void FlushCache()
    {
        if (write_buffer_size_ == 0) return;

        std::memcpy(disk_->data() + write_buffer_start_, write_buffer_.get(), write_buffer_size_);
        write_buffer_size_ = 0;
    }

private:

    void NeedReadCache()
    {
        if (read_buffer_) return;
        read_buffer_.reset(new uint8_t[read_ahead]);
        read_buffer_start_ = -1;
        read_buffer_size_ = 0;
    }

    void NeedWriteCache()
    {
        if (write_buffer_) return;
        write_buffer_.reset(new uint8_t[write_cache]);
        write_buffer_start_ = -1;
        write_buffer_size_ = 0;
    }

    std::vector<uint8_t>* disk_;

    uint64_t file_size_;

    // the file offset the read buffer was read from
    uint64_t read_buffer_start_ = -1;
    std::unique_ptr<uint8_t[]> read_buffer_;
    uint64_t read_buffer_size_ = 0;

    // the file offset the write buffer should be written back to
    // the write buffer is *only* for contiguous and sequential writes
    uint64_t write_buffer_start_ = -1;
    std::unique_ptr<uint8_t[]> write_buffer_;
    uint64_t write_buffer_size_ = 0;
};

struct FilteredDisk : Disk
{
    FilteredDisk(BufferedDisk underlying, bitfield filter, int entry_size)
        : filter_(std::move(filter))
        , underlying_(std::move(underlying))
        , entry_size_(entry_size)
    {
        assert(entry_size_ > 0);
        while (!filter_.get(last_idx_)) {
            last_physical_ += entry_size_;
            ++last_idx_;
        }
        assert(filter_.get(last_idx_));
        assert(last_physical_ == last_idx_ * entry_size_);
    }

    uint8_t const* Read(uint64_t begin, uint64_t length) override
    {
        // we only support a single read-pass with no going backwards
        assert(begin >= last_logical_);
        assert((begin % entry_size_) == 0);
        assert(filter_.get(last_idx_));
        assert(last_physical_ == last_idx_ * entry_size_);

        if (begin > last_logical_) {
            // last_idx_ et.al. always points to an entry we have (i.e. the bit
            // is set). So when we advance from there, we always take at least
            // one step on all counters.
            last_logical_ += entry_size_;
            last_physical_ += entry_size_;
            ++last_idx_;

            while (begin > last_logical_)
            {
                if (filter_.get(last_idx_)) {
                    last_logical_ += entry_size_;
                }
                last_physical_ += entry_size_;
                ++last_idx_;
            }

            while (!filter_.get(last_idx_)) {
                last_physical_ += entry_size_;
                ++last_idx_;
            }
        }

        assert(filter_.get(last_idx_));
        assert(last_physical_ == last_idx_ * entry_size_);
        assert(begin == last_logical_);
        return underlying_.Read(last_physical_, length);
    }

    void Write(uint64_t begin, const uint8_t *memcache, uint64_t length) override
    {
        assert(false);
        throw std::runtime_error("Write() called on read-only disk abstraction");
    }
    void Truncate(uint64_t new_size) override
    {
        underlying_.Truncate(new_size);
        if (new_size == 0) filter_.free_memory();
    }
    std::string GetFileName() override { return underlying_.GetFileName(); }
    void FreeMemory() override
    {
        filter_.free_memory();
        underlying_.FreeMemory();
    }

private:

    // only entries whose bit is set should be read
    bitfield filter_;
    BufferedDisk underlying_;
    int entry_size_;

    // the "physical" disk offset of the last read
    uint64_t last_physical_ = 0;
    // the "logical" disk offset of the last read. i.e. the offset as if the
    // file would have been compacted based on filter_
    uint64_t last_logical_ = 0;

    // the index of the last read. This is also the index into the bitfield. It
    // could be computed as last_physical_ / entry_size_, but we want to avoid
    // the division.
    uint64_t last_idx_ = 0;
};

#endif  // SRC_CPP_DISK_HPP_
