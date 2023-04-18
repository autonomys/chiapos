#pragma once

#include "plotter.hpp"
#include "prover.hpp"
#include "verifier.hpp"

// Create new table for K with 32 bytes seed
extern "C" std::vector<uint8_t>* subspace_chiapos_create_table(uint8_t k, const uint8_t* seed) {
    return Plotter().CreatePlot(k, seed, 10, 0, 4000);
}

extern "C" void subspace_chiapos_free_table(std::vector<uint8_t>* table) {
    delete table;
}

extern "C" Prover* subspace_chiapos_create_prover(const std::vector<uint8_t>* table) {
    return new Prover(table);
}

extern "C" void subspace_chiapos_free_prover(Prover* prover) {
    delete prover;
}

// Prover is the same as created by `create_prover` above.
//
// On success writes `32` bytes and returns `true`, returns `false` otherwise.
extern "C" bool subspace_chiapos_find_quality(const Prover* prover, uint32_t challenge_index, uint8_t* quality) {
    uint8_t challenge[32] = {0};
    std::memcpy(challenge, &challenge_index, sizeof challenge_index);
    try {
        // TODO: Potentially optimize `GetQualitiesForChallenge` to check for existing of the first
        //  quality rather than scanning for all of them
        auto qualities = prover->GetQualitiesForChallenge(challenge);
        if (!qualities.empty()) {
            qualities.front().ToBytes(quality);
            return true;
        }

        return false;
    } catch (...) {
        return false;
    }
}

// Prover is the same as created by `create_prover` above.
//
// On success writes `k*8` bytes and returns `true`, returns `false` otherwise.
extern "C" bool subspace_chiapos_create_proof(const Prover* prover, uint32_t challenge_index, uint8_t* proof) {
    uint8_t challenge[32] = {0};
    std::memcpy(challenge, &challenge_index, sizeof challenge_index);
    try {
        prover->GetFullProof(challenge, 0).ToBytes(proof);
        return true;
    } catch (...) {
        return false;
    }
}

// Check if proof is valid for K with 32 byte seed and challenge/proof form `create_proof` above.
extern "C" bool subspace_chiapos_is_proof_valid(
    uint8_t k,
    const uint8_t* seed,
    uint32_t challenge_index,
    const uint8_t* proof,
    uint8_t* quality
) {
    uint8_t challenge[32] = {0};
    std::memcpy(challenge, &challenge_index, sizeof challenge_index);
    try {
        auto found_quality = Verifier::ValidateProof(k,seed, challenge, proof, k * 8);

        if (found_quality.GetSize() != 0) {
            // This is already done internally in `Verifier::ValidateProof` above, but in order to
            // not change its public API we're doing it again here
            Bits challenge_bits = Bits(challenge, 256 / 8, 256);
            uint16_t quality_index = challenge_bits.Slice(256 - 5).GetValue() << 1;

            if (quality_index == 0) {
                found_quality.ToBytes(quality);
                return true;
            }
        }

        return false;
    } catch (...) {
        return false;
    }
}
