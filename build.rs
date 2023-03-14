use std::env;

fn main() {
    if env::var("DOCS_RS").is_ok() {
        // Skip everything when building docs on docs.rs
        return;
    }

    let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap();
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_env = env::var("CARGO_CFG_TARGET_ENV").unwrap();

    {
        let mut cc = cc::Build::new();

        cc.warnings(false).files(&[
            "lib/FiniteStateEntropy/lib/fse_compress.c",
            "lib/FiniteStateEntropy/lib/fse_decompress.c",
            "lib/FiniteStateEntropy/lib/entropy_common.c",
            "lib/FiniteStateEntropy/lib/hist.c",
            "src/chacha8.c",
        ]);

        if target_env == "msvc" {
            cc.files(&[
                "src/b3/blake3.c",
                "src/b3/blake3_portable.c",
                "src/b3/blake3_dispatch.c",
                "src/b3/blake3_avx2.c",
                "src/b3/blake3_avx512.c",
                "src/b3/blake3_sse41.c",
            ]);
        } else if target_os == "macos" && target_arch == "aarch64" {
            cc.files(&[
                "src/b3/blake3.c",
                "src/b3/blake3_portable.c",
                "src/b3/blake3_dispatch.c",
            ]);
        } else {
            cc.files(&[
                "src/b3/blake3.c",
                "src/b3/blake3_portable.c",
                "src/b3/blake3_dispatch.c",
                "src/b3/blake3_avx2_x86-64_unix.S",
                "src/b3/blake3_avx512_x86-64_unix.S",
                "src/b3/blake3_sse41_x86-64_unix.S",
            ]);
        }

        cc.compile("subspace_chiapos_deps");
    }

    {
        let mut cc = cc::Build::new();
        cc.cpp(true)
            .flag_if_supported("/std:c++17")
            .flag_if_supported("-std=c++17")
            .warnings(false)
            .file("src/lib.cpp");

        if target_env == "msvc" {
            cc.file("uint128_t/uint128_t.cpp").include("uint128_t");
        }

        cc.compile("subspace_chiapos");
    }
}
