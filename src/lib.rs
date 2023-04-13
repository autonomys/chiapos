//! Chia Proof of Space adapted for Subspace needs
#![warn(rust_2018_idioms, missing_debug_implementations, missing_docs)]

// Brings in FSE's `entropy_common.c`, which otherwise would cause linking issues
#[allow(unused_imports)]
use zstd_sys::*;

const K: u8 = 17;

/// Abstraction that represents quality of the solution in the table
#[derive(Debug)]
pub struct Quality<'a> {
    bytes: [u8; 32],
    challenge_index: u32,
    table: &'a Table,
}

impl<'a> Quality<'a> {
    /// Get underlying bytes representation of the quality
    pub fn to_bytes(&self) -> [u8; 32] {
        self.bytes
    }

    /// Create proof for this solution
    pub fn create_proof(&self) -> [u8; K as usize * 8] {
        let mut bytes = [0; K as usize * 8];
        // SAFETY: Called with valid prover and pointer to memory with correct size
        let success = unsafe {
            ffi::subspace_chiapos_create_proof(
                self.table.prover,
                self.challenge_index,
                bytes.as_mut_ptr(),
            )
        };
        assert!(
            success,
            "Must succeed, we have just checked quality exists; qed"
        );

        bytes
    }
}

/// Data structure essentially representing Chia's plot table
#[derive(Debug)]
pub struct Table {
    table: ffi::Table,
    prover: ffi::Prover,
}

impl Drop for Table {
    fn drop(&mut self) {
        // SAFETY: Called exactly once on correctly allocated pointer
        unsafe {
            ffi::subspace_chiapos_free_prover(self.prover);
            ffi::subspace_chiapos_free_table(self.table);
        }
    }
}

impl Table {
    /// Generate new table with 32 bytes seed
    pub fn generate(seed: &[u8; 32]) -> Self {
        // SAFETY: Called with correctly sized seed
        let table = unsafe { ffi::subspace_chiapos_create_table(K, seed.as_ptr()) };
        // SAFETY: Called with correctly created table and lifetime of table is longer than of
        // prover itself
        let prover = unsafe { ffi::subspace_chiapos_create_prover(table) };
        Self { table, prover }
    }

    /// Try to find quality of the proof at `challenge_index` if proof exists
    pub fn find_quality(&self, challenge_index: u32) -> Option<Quality<'_>> {
        let mut bytes = [0u8; 32];
        // SAFETY: Called with prover that is still alive
        unsafe {
            ffi::subspace_chiapos_find_quality(self.prover, challenge_index, bytes.as_mut_ptr())
        }
        .then_some(Quality {
            bytes,
            challenge_index,
            table: self,
        })
    }
}

/// Check whether proof created earlier is valid
pub fn is_proof_valid(seed: &[u8; 32], challenge_index: u32, proof: &[u8; K as usize * 8]) -> bool {
    // SAFETY: Called with valid pointer to seed and proof with correct size
    unsafe {
        ffi::subspace_chiapos_is_proof_valid(K, seed.as_ptr(), challenge_index, proof.as_ptr())
    }
}

mod ffi {
    use std::ffi::c_void;

    #[repr(transparent)]
    #[derive(Debug, Copy, Clone)]
    pub struct Table(*const c_void);

    unsafe impl Send for Table {}
    unsafe impl Sync for Table {}

    #[repr(transparent)]
    #[derive(Debug, Copy, Clone)]
    pub struct Prover(*const c_void);

    unsafe impl Send for Prover {}
    unsafe impl Sync for Prover {}

    extern "C" {
        // Create new table for K with 32 bytes seed
        pub(super) fn subspace_chiapos_create_table(k: u8, seed: *const u8) -> Table;

        pub(super) fn subspace_chiapos_free_table(table: Table);

        pub(super) fn subspace_chiapos_create_prover(table: Table) -> Prover;

        pub(super) fn subspace_chiapos_free_prover(prover: Prover);

        // Prover is the same as created by `create_prover` above.
        //
        // On success writes `32` bytes and returns `true`, returns `false` otherwise.
        pub(super) fn subspace_chiapos_find_quality(
            prover: Prover,
            challenge_index: u32,
            quality: *mut u8,
        ) -> bool;

        // Prover is the same as created by `create_prover` above.
        //
        // On success writes `k*8` bytes and returns `true`, returns `false` otherwise.
        pub(super) fn subspace_chiapos_create_proof(
            prover: Prover,
            challenge_index: u32,
            proof: *mut u8,
        ) -> bool;

        // Check if proof is valid for K with 32 byte seed and challenge/proof form `create_proof` above.
        pub(super) fn subspace_chiapos_is_proof_valid(
            k: u8,
            seed: *const u8,
            challenge_index: u32,
            proof: *const u8,
        ) -> bool;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SEED: [u8; 32] = [
        35, 2, 52, 4, 51, 55, 23, 84, 91, 10, 111, 12, 13, 222, 151, 16, 228, 211, 254, 45, 92,
        198, 204, 10, 9, 10, 11, 129, 139, 171, 15, 23,
    ];

    #[test]
    fn basic() {
        let table = Table::generate(&SEED);

        assert!(table.find_quality(0).is_none());

        {
            let challenge_index = 1;
            let quality = table.find_quality(challenge_index).unwrap();
            let proof = quality.create_proof();
            assert!(is_proof_valid(&SEED, challenge_index, &proof));
        }
    }
}
