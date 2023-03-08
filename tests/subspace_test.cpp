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

#define _PRINT_LOGS

#include <stdio.h>

#include <catch2/catch.hpp>
#include "plotter_disk.hpp"
#include "prover_disk.hpp"
#include "verifier.hpp"

using namespace std;

uint8_t plot_id_1[] = {35,  2,   52,  4,  51, 55,  23,  84, 91, 10, 111, 12,  13,  222, 151, 16,
                       228, 211, 254, 45, 92, 198, 204, 10, 9,  10, 11,  129, 139, 171, 15,  23};

vector<unsigned char> intToBytes(uint32_t paramInt, uint32_t numBytes)
{
    vector<unsigned char> arrayOfByte(numBytes, 0);
    for (uint32_t i = 0; paramInt > 0; i++) {
        arrayOfByte[numBytes - i - 1] = paramInt & 0xff;
        paramInt >>= 8;
    }
    return arrayOfByte;
}

void HexToBytes(const string& hex, uint8_t* result)
{
    for (unsigned int i = 0; i < hex.length(); i += 2) {
        string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t)strtol(byteString.c_str(), NULL, 16);
        result[i / 2] = byte;
    }
}

void TestProofOfSpace(
    const std::vector<uint8_t>* plot,
    uint32_t iterations,
    uint8_t k,
    uint8_t* plot_id,
    uint32_t num_proofs)
{
    DiskProver prover(plot);
    auto* proof_data = new uint8_t[8 * k];
    uint32_t success = 0;
    // Tries an edge case challenge with many 1s in the front, and ensures there is no segfault
    vector<unsigned char> hash(picosha2::k_digest_size);
    HexToBytes("fffffa2b647d4651c500076d7df4c6f352936cf293bd79c591a7b08e43d6adfb", hash.data());
    prover.GetQualitiesForChallenge(hash.data());

    for (uint32_t i = 0; i < iterations; i++) {
        vector<unsigned char> hash_input = intToBytes(i, 4);
        vector<unsigned char> hash(picosha2::k_digest_size);
        picosha2::hash256(hash_input.begin(), hash_input.end(), hash.begin(), hash.end());
        vector<LargeBits> qualities = prover.GetQualitiesForChallenge(hash.data());
        Verifier verifier = Verifier();

        for (uint32_t index = 0; index < qualities.size(); index++) {
            LargeBits proof = prover.GetFullProof(hash.data(), index);
            proof.ToBytes(proof_data);

            LargeBits quality = verifier.ValidateProof(plot_id, k, hash.data(), proof_data, k * 8);
            REQUIRE(quality.GetSize() == 256);
            REQUIRE(quality == qualities[index]);
            success += 1;

            // Tests invalid proof
            proof_data[0] = (proof_data[0] + 1) % 256;
            LargeBits quality_2 =
                verifier.ValidateProof(plot_id, k, hash.data(), proof_data, k * 8);
            REQUIRE(quality_2.GetSize() == 0);
        }
    }
    std::cout << "Success: " << success << "/" << iterations << " "
              << (100 * ((double)success / (double)iterations)) << "%" << std::endl;
    REQUIRE(success == num_proofs);
    REQUIRE(success > 0.5 * iterations);
    REQUIRE(success < 1.5 * iterations);
    delete[] proof_data;
}

void PlotAndTestProofOfSpace(
    uint32_t iterations,
    uint8_t k,
    uint8_t* plot_id,
    uint32_t buffer,
    uint32_t num_proofs,
    uint32_t stripe_size)
{
    DiskPlotter plotter = DiskPlotter();

    auto* plot = plotter.CreatePlotDisk(
        k,
        plot_id,
        32,
        buffer,
        0,
        stripe_size
    );

    TestProofOfSpace(plot, iterations, k, plot_id, num_proofs);

    delete plot;
}

TEST_CASE("Plotting")
{
    PlotAndTestProofOfSpace(100, 17, plot_id_1, 11, 93, 2000);
}


