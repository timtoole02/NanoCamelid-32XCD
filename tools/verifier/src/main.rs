//! nc-verifier — token-parity check: console-generated token IDs vs the
//! Rust reference (Milestone 7+; file-vs-file core implemented now).
//!
//! reference.bin format (from nc-reference --eval):
//!   u16 prompt_count, then per prompt:
//!     u16 prompt_len, prompt_len * u16 ids,
//!     u16 output_len, output_len * u16 ids,
//!     u16 fallback_level                           (all big-endian)
//!
//! console.bin format (the console token buffer dump, memory-map.md):
//!   u16 count, count * u16 ids                    (big-endian)
//!
//! Usage: nc-verifier REFERENCE.bin PROMPT_INDEX CONSOLE.bin

fn be16(b: &[u8], p: usize) -> u16 {
    u16::from_be_bytes([b[p], b[p + 1]])
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 4 {
        eprintln!("usage: nc-verifier REFERENCE.bin PROMPT_INDEX CONSOLE.bin");
        std::process::exit(2);
    }
    let refbuf = std::fs::read(&args[1]).expect("read reference");
    let want_idx: usize = args[2].parse().expect("prompt index");
    let conbuf = std::fs::read(&args[3]).expect("read console dump");

    let n = be16(&refbuf, 0) as usize;
    assert!(want_idx < n, "prompt index {want_idx} out of range ({n} prompts)");
    let mut p = 2usize;
    let mut ref_ids: Vec<u16> = Vec::new();
    for i in 0..=want_idx {
        let plen = be16(&refbuf, p) as usize;
        p += 2 + plen * 2;
        let olen = be16(&refbuf, p) as usize;
        p += 2;
        if i == want_idx {
            ref_ids = (0..olen).map(|j| be16(&refbuf, p + j * 2)).collect();
        }
        p += olen * 2 + 2; /* + fallback word */
    }

    let ccount = be16(&conbuf, 0) as usize;
    let con_ids: Vec<u16> = (0..ccount).map(|j| be16(&conbuf, 2 + j * 2)).collect();

    let matching = ref_ids == con_ids;
    println!("reference[{want_idx}]: {ref_ids:?}");
    println!("console:      {con_ids:?}");
    if matching {
        println!("PARITY PASS ({} tokens)", ref_ids.len());
    } else {
        let first_diff = ref_ids
            .iter()
            .zip(con_ids.iter())
            .position(|(a, b)| a != b)
            .unwrap_or(ref_ids.len().min(con_ids.len()));
        println!("PARITY FAIL (first divergence at token {first_diff})");
        std::process::exit(1);
    }
}
