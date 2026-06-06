//! nc-packer — quantize + pack the trained model into the console binary
//! set (big-endian) + manifest.json with SHA-256 hashes (Milestone 4).
//!
//! Inputs: corpus.txt (tables are re-derived deterministically) and
//! trained.ncm (float weights from nc-trainer; vocab must match).
//! Outputs in MODELDIR: vocab.bin tokenizer.bin candidates.bin weights.bin
//! decode.bin manifest.json

use nc_model::{pack, train, *};
use std::process::Command;

fn read_ncm(path: &str, vocab: &Vocab) -> train::FloatModel {
    let buf = std::fs::read(path).expect("read ncm");
    assert_eq!(&buf[0..4], b"NCM1", "bad magic");
    let mut p = 4usize;
    let rd_u32 = |b: &[u8], p: &mut usize| -> u32 {
        let v = u32::from_le_bytes(b[*p..*p + 4].try_into().unwrap());
        *p += 4;
        v
    };
    let n = rd_u32(&buf, &mut p) as usize;
    assert_eq!(n, vocab.len(), "vocab size mismatch vs corpus");
    for w in &vocab.words {
        let l = rd_u32(&buf, &mut p) as usize;
        let s = std::str::from_utf8(&buf[p..p + l]).unwrap();
        assert_eq!(s, w, "vocab word mismatch — retrain with current corpus");
        p += l;
    }
    let rd_f32 = |b: &[u8], p: &mut usize| -> f32 {
        let v = f32::from_le_bytes(b[*p..*p + 4].try_into().unwrap());
        *p += 4;
        v
    };
    let rd_emb = |b: &[u8], p: &mut usize| -> Vec<[f32; DIM]> {
        (0..n)
            .map(|_| {
                let mut a = [0f32; DIM];
                for x in a.iter_mut() {
                    *x = rd_f32(b, p);
                }
                a
            })
            .collect()
    };
    let ctx_emb = rd_emb(&buf, &mut p);
    let cand_emb = rd_emb(&buf, &mut p);
    let bias: Vec<f32> = (0..n).map(|_| rd_f32(&buf, &mut p)).collect();
    let mut qtype_emb = [[0f32; DIM]; QT_COUNT];
    for q in qtype_emb.iter_mut() {
        for x in q.iter_mut() {
            *x = rd_f32(&buf, &mut p);
        }
    }
    assert_eq!(p, buf.len(), "trailing bytes in ncm");
    train::FloatModel { vocab_len: n, ctx_emb, cand_emb, bias, qtype_emb }
}

fn sha256_file(path: &str) -> String {
    let out = Command::new("shasum").args(["-a", "256", path]).output().expect("shasum");
    String::from_utf8(out.stdout).unwrap().split_whitespace().next().unwrap().to_string()
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 4 {
        eprintln!("usage: nc-packer CORPUS.TXT TRAINED.ncm MODELDIR");
        std::process::exit(2);
    }
    let (corpus_path, ncm_path, dir) = (&args[1], &args[2], &args[3]);
    let corpus = std::fs::read_to_string(corpus_path).expect("read corpus");
    let lines: Vec<Line> = corpus.lines().filter_map(parse_line).collect();
    let vocab = build_vocab(&lines);
    let seqs: Vec<Vec<u16>> = lines.iter().map(|l| line_tokens(&vocab, l)).collect();
    let tables = build_ngrams(vocab.len(), &seqs);
    let fm = read_ncm(ncm_path, &vocab);
    let qm = train::quantize(&fm);

    // score-magnitude bound check (no i32 overflow headroom surprises)
    let max_cv = (CTX_W.iter().sum::<i32>() + QT_W) * 127;
    let max_dot = max_cv * 127 * DIM as i32;
    assert!(max_dot >> qm.sh_dot < (1 << 24), "dot term too large");

    std::fs::create_dir_all(dir).unwrap();
    let files = [
        ("vocab.bin", pack::vocab_bin(&vocab)),
        ("tokenizer.bin", pack::tokenizer_bin(&vocab)),
        ("candidates.bin", pack::candidates_bin(vocab.len(), &tables)),
        ("weights.bin", pack::weights_bin(&qm)),
        ("decode.bin", pack::decode_bin()),
    ];
    let mut entries = Vec::new();
    for (name, data) in &files {
        let path = format!("{dir}/{name}");
        std::fs::write(&path, data).unwrap();
        entries.push((name.to_string(), data.len(), sha256_file(&path)));
        println!("{name}: {} bytes", data.len());
    }
    let corpus_sha = sha256_file(corpus_path);

    let mut manifest = String::from("{\n  \"format_version\": 1,\n");
    manifest.push_str(&format!("  \"vocab_len\": {},\n", vocab.len()));
    manifest.push_str(&format!(
        "  \"dim\": {DIM}, \"ctx\": {CTX}, \"k\": {K}, \"max_tokens\": {MAX_TOKENS},\n"
    ));
    manifest.push_str(&format!(
        "  \"sh_dot\": {}, \"sh_b\": {}, \"sh_ng\": {SH_NG}, \"rep_pen\": {REP_PEN},\n",
        qm.sh_dot, qm.sh_b
    ));
    manifest.push_str(&format!("  \"corpus_sha256\": \"{corpus_sha}\",\n"));
    manifest.push_str("  \"quantization\": \"int8 per-table max-abs, power-of-2 shifts\",\n");
    manifest.push_str("  \"files\": {\n");
    for (i, (name, len, sha)) in entries.iter().enumerate() {
        manifest.push_str(&format!(
            "    \"{name}\": {{ \"bytes\": {len}, \"sha256\": \"{sha}\" }}{}\n",
            if i + 1 < entries.len() { "," } else { "" }
        ));
    }
    manifest.push_str("  }\n}\n");
    std::fs::write(format!("{dir}/manifest.json"), &manifest).unwrap();
    println!("manifest.json written");
}
