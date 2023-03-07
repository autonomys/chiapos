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

#include <vector>

#include "chia_filesystem.hpp"

#include "bitfield.hpp"

struct Disk {
    virtual uint8_t const* Read(uint64_t begin, uint64_t length) = 0;
    virtual void Truncate(uint64_t new_size) = 0;
    virtual void FreeMemory() = 0;
    virtual ~Disk() = default;
};

struct BufferedDisk : Disk
{
    BufferedDisk(std::vector<uint8_t>* disk) : disk_(disk) {}

    uint8_t const* Read(uint64_t begin, uint64_t length) override
    {
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

    void Truncate(uint64_t const new_size) override
    {
        disk_->resize(new_size);
    }

    void FreeMemory() override
    {
    }

private:

    std::vector<uint8_t>* disk_;
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

    void Truncate(uint64_t new_size) override
    {
        underlying_.Truncate(new_size);
        if (new_size == 0) filter_.free_memory();
    }
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
