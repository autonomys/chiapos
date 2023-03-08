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

#ifndef SRC_CPP_PLOTTER_DISK_HPP_
#define SRC_CPP_PLOTTER_DISK_HPP_

#ifndef _WIN32
#include <semaphore.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <stdio.h>
#include <sys/stat.h>

#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "calculate_bucket.hpp"
#include "encoding.hpp"
#include "exceptions.hpp"
#include "phases.hpp"
#include "phase1.hpp"
#include "phase2.hpp"
#include "phase3.hpp"
#include "phase4.hpp"
#include "pos_constants.hpp"
#include "sort_manager.hpp"
#include "util.hpp"

class DiskPlotter {
public:
    // This method creates a plot on disk with the filename. Many temporary files
    // (filename + ".table1.tmp", filename + ".p2.t3.sort_bucket_4.tmp", etc.) are created
    // and their total size will be larger than the final plot file. Temp files are deleted at the
    // end of the process.
    std::vector<uint8_t>* CreatePlotDisk(
        uint8_t k,
        const uint8_t* id,
        uint32_t id_len,
        uint32_t buf_megabytes_input = 0,
        uint32_t num_buckets_input = 0,
        uint64_t stripe_size_input = 0,
        uint8_t phases_flags = ENABLE_BITFIELD)
    {
        if (k < kMinPlotSize || k > kMaxPlotSize) {
            throw InvalidValueException("Plot size k= " + std::to_string(k) + " is invalid");
        }

        uint32_t stripe_size, buf_megabytes, num_buckets;
        if (stripe_size_input != 0) {
            stripe_size = stripe_size_input;
        } else {
            stripe_size = 65536;
        }
        if (buf_megabytes_input != 0) {
            buf_megabytes = buf_megabytes_input;
        } else {
            buf_megabytes = 4608;
        }

        if (buf_megabytes < 10) {
            throw InsufficientMemoryException("Please provide at least 10MiB of ram");
        }

        // Subtract some ram to account for dynamic allocation through the code
        uint64_t thread_memory = (2 * (stripe_size + 5000)) *
                                 EntrySizes::GetMaxEntrySize(k, 4, true) / (1024 * 1024);
        uint64_t sub_mbytes = (5 + (int)std::min(buf_megabytes * 0.05, (double)50) + thread_memory);
        if (sub_mbytes > buf_megabytes) {
            throw InsufficientMemoryException(
                "Please provide more memory. At least " + std::to_string(sub_mbytes));
        }
        uint64_t memory_size = ((uint64_t)(buf_megabytes - sub_mbytes)) * 1024 * 1024;
        double max_table_size = 0;
        for (size_t i = 1; i <= 7; i++) {
            double memory_i = 1.3 * ((uint64_t)1 << k) * EntrySizes::GetMaxEntrySize(k, i, true);
            if (memory_i > max_table_size)
                max_table_size = memory_i;
        }
        if (num_buckets_input != 0) {
            num_buckets = Util::RoundPow2(num_buckets_input);
        } else {
            num_buckets = 2 * Util::RoundPow2(ceil(
                                  ((double)max_table_size) / (memory_size * kMemSortProportion)));
        }

        if (num_buckets < kMinBuckets) {
            if (num_buckets_input != 0) {
                throw InvalidValueException("Minimum buckets is " + std::to_string(kMinBuckets));
            }
            num_buckets = kMinBuckets;
        } else if (num_buckets > kMaxBuckets) {
            if (num_buckets_input != 0) {
                throw InvalidValueException("Maximum buckets is " + std::to_string(kMaxBuckets));
            }
            double required_mem =
                (max_table_size / kMaxBuckets) / kMemSortProportion / (1024 * 1024) + sub_mbytes;
            throw InsufficientMemoryException(
                "Do not have enough memory. Need " + std::to_string(required_mem) + " MiB");
        }
        uint32_t log_num_buckets = log2(num_buckets);
        assert(log2(num_buckets) == ceil(log2(num_buckets)));

        if (max_table_size / num_buckets < stripe_size * 30) {
            throw InvalidValueException("Stripe size too large");
        }

#if defined(_WIN32) || defined(__x86_64__)
        if (phases_flags & ENABLE_BITFIELD && !Util::HavePopcnt()) {
            throw InvalidValueException("Bitfield plotting not supported by CPU");
        }
#endif /* defined(_WIN32) || defined(__x86_64__) */

#ifdef _PRINT_LOGS
        std::cout << std::endl << "Starting plotting progress" << std::endl;
        std::cout << "ID: " << Util::HexStr(id, id_len) << std::endl;
        std::cout << "Plot size is: " << static_cast<int>(k) << std::endl;
        std::cout << "Buffer size is: " << buf_megabytes << "MiB" << std::endl;
        std::cout << "Using " << num_buckets << " buckets" << std::endl;
        std::cout << "Using 1 thread of stripe size " << stripe_size << std::endl;
        std::cout << "Process ID is: " << ::getpid() << std::endl;
#endif

        auto tmp2_vector = new std::vector<uint8_t>();

        {
            // Scope for FileDisk
            std::vector<std::vector<uint8_t>> tmp_1_vectors;
            // The table0 file will be used for sort on disk spare. tables 1-7 are stored in their
            // own vector.
            tmp_1_vectors.emplace_back();
            for (size_t i = 1; i <= 7; i++) {
                tmp_1_vectors.emplace_back();
            }
            // TODO: Would be nice to preallocate
            // for (auto const& vec : tmp_1_vectors) {
            //     vec.reserve(?);
            // }

            assert(id_len == kIdLen);

#ifdef _PRINT_LOGS
            std::cout << std::endl
                      << "Starting phase 1/4: Forward Propagation... "
                      << Timer::GetNow();
            Timer p1;
            Timer all_phases;
#endif

            std::vector<uint64_t> table_sizes = RunPhase1(
                tmp_1_vectors,
                k,
                id,
                memory_size,
                num_buckets,
                log_num_buckets,
                stripe_size,
                phases_flags);
#ifdef _PRINT_LOGS
            p1.PrintElapsed("Time for phase 1 =");

            uint64_t finalsize=0;
#endif

#ifdef _PRINT_LOGS
            std::cout << std::endl
                  << "Starting phase 2/4: Backpropagation... "
                  << Timer::GetNow();
            Timer p2;
#endif

            Phase2Results res2 = RunPhase2(
                tmp_1_vectors,
                table_sizes,
                k,
                memory_size,
                num_buckets,
                log_num_buckets,
                phases_flags);
#ifdef _PRINT_LOGS
            p2.PrintElapsed("Time for phase 2 =");
#endif

            // Now we open a new file, where the final contents of the plot will be stored.
            uint32_t header_size = WriteHeader(tmp2_vector, k, id);

#ifdef _PRINT_LOGS
            std::cout << std::endl
                  << "Starting phase 3/4: Compression... " << Timer::GetNow();
            Timer p3;
#endif
            Phase3Results res = RunPhase3(
                k,
                tmp2_vector,
                std::move(res2),
                id,
                header_size,
                memory_size,
                num_buckets,
                log_num_buckets,
                phases_flags);
#ifdef _PRINT_LOGS
            p3.PrintElapsed("Time for phase 3 =");
#endif

#ifdef _PRINT_LOGS
            std::cout << std::endl
                  << "Starting phase 4/4: Write Checkpoint tables... " << Timer::GetNow();
            Timer p4;
#endif
            RunPhase4(k, k + 1, tmp2_vector, res, phases_flags, 16);
#ifdef _PRINT_LOGS
            p4.PrintElapsed("Time for phase 4 =");
            finalsize = res.final_table_begin_pointers[11];
#endif

            // The total number of bytes used for sort is saved to table_sizes[0]. All other
            // elements in table_sizes represent the total number of entries written by the end of
            // phase 1 (which should be the highest total working space time). Note that the max
            // sort on disk space does not happen at the exact same time as max table sizes, so this
            // estimate is conservative (high).
            uint64_t total_working_space = table_sizes[0];
            for (size_t i = 1; i <= 7; i++) {
                total_working_space += table_sizes[i] * EntrySizes::GetMaxEntrySize(k, i, false);
            }
#ifdef _PRINT_LOGS
            std::cout << "Approximate working space used (without final file): "
                      << static_cast<double>(total_working_space) / (1024 * 1024 * 1024) << " GiB"
                      << std::endl;

            std::cout << "Final File size: "
                      << static_cast<double>(finalsize) /
                             (1024 * 1024 * 1024)
                      << " GiB" << std::endl;
            all_phases.PrintElapsed("Total time =");
#endif
        }

        return tmp2_vector;
    }

private:
    // Writes the plot file header to a file
    uint32_t WriteHeader(
        std::vector<uint8_t>* plot_vector,
        uint8_t k,
        const uint8_t* id)
    {
        // 19 bytes  - "Proof of Space Plot" (utf-8)
        // 32 bytes  - unique plot id
        // 1 byte    - k
        // 2 bytes   - format description length
        // x bytes   - format description

        std::string header_text = "Proof of Space Plot";
        uint64_t write_pos = 0;
        plot_vector->insert(plot_vector->end(), header_text.begin(), header_text.end());
        write_pos += header_text.size();

        write_pos += write_to_vector_at(plot_vector, write_pos, id, kIdLen);

        uint8_t k_buffer[1];
        k_buffer[0] = k;
        write_pos += write_to_vector_at(plot_vector, write_pos, k_buffer, 1);

        uint8_t size_buffer[2];
        Util::IntToTwoBytes(size_buffer, kFormatDescription.size());
        write_pos += write_to_vector_at(plot_vector, write_pos, size_buffer, 2);
        write_pos += write_to_vector_at(
            plot_vector, write_pos,
            (uint8_t*)kFormatDescription.data(),
            kFormatDescription.size());

        uint8_t pointers[10 * 8];
        memset(pointers, 0, 10 * 8);
        write_pos += write_to_vector_at(plot_vector, write_pos, pointers, 10 * 8);

#ifdef _PRINT_LOGS
        std::cout << "Wrote: " << write_pos << std::endl;
#endif
        return write_pos;
    }
};

#endif  // SRC_CPP_PLOTTER_DISK_HPP_
