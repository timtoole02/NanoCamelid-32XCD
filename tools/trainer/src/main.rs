//! nc-trainer — offline training for NanoCamelid 32XCD (Milestone 4).
//!
//! corpus.txt -> deterministic SGD on the candidate reranker -> trained.ncm
//! (float weights, host-only artifact; the packer quantizes and packs).
//!
//! Fully deterministic: seeded xorshift PRNG, fixed sample order, no system
//! entropy, no parallelism.

use nc_model::{train, *};
use std::io::Write;

const EPOCHS: usize = 400;
const LR0: f32 = 0.08;
const SEED: u32 = 0x32584344; // "2XCD"

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: nc-trainer CORPUS.TXT OUT.ncm");
        std::process::exit(2);
    }
    let corpus = std::fs::read_to_string(&args[1]).expect("read corpus");
    let lines: Vec<Line> = corpus.lines().filter_map(parse_line).collect();
    let vocab = build_vocab(&lines);
    assert!(vocab.len() <= VOCAB_MAX, "vocab overflow: {}", vocab.len());

    let seqs: Vec<(Vec<u16>, usize)> =
        lines.iter().map(|l| (line_tokens(&vocab, l), l.qtype)).collect();
    let tables = build_ngrams(vocab.len(), &seqs.iter().map(|(s, _)| s.clone()).collect::<Vec<_>>());
    let (samples, total, missed) = train::build_samples(&seqs, &tables);
    println!(
        "corpus: {} lines, vocab {} words, {} tri contexts",
        lines.len(), vocab.len(), tables.tri.len()
    );
    println!(
        "samples: {} usable / {} total ({} gold-not-in-candidates, {:.1}% coverage)",
        samples.len(), total, missed,
        100.0 * samples.len() as f64 / total.max(1) as f64
    );

    let mut rng = train::Rng(SEED);
    let mut model = train::FloatModel::init(vocab.len(), &mut rng);
    let mut lr = LR0;
    for e in 0..EPOCHS {
        let loss = train::epoch(&mut model, &samples, lr);
        lr *= 0.995;
        if e % 50 == 0 || e == EPOCHS - 1 {
            println!("epoch {e:3}  loss {loss:.4}  lr {lr:.4}");
        }
    }

    // advisory: greedy accuracy on training samples (quantized model)
    let q = train::quantize(&model);
    let mut correct = 0usize;
    for s in &samples {
        let cv = context_vector(&q, &s.ctx, s.qt);
        let mut best = 0usize;
        let mut best_s = SCORE_MIN;
        for (i, (tok, ng)) in s.cands.iter().enumerate() {
            // teacher-forced: no repetition context, emitted "enough"
            let sc = score_candidate(&q, &cv, *tok, *ng, &[], MIN_TOKENS);
            if sc > best_s {
                best_s = sc;
                best = i;
            }
        }
        if best == s.gold_idx {
            correct += 1;
        }
    }
    println!(
        "quantized teacher-forced accuracy: {}/{} ({:.1}%)  sh_dot={} sh_b={}",
        correct, samples.len(),
        100.0 * correct as f64 / samples.len().max(1) as f64,
        q.sh_dot, q.sh_b
    );

    // trained.ncm: magic, vocab words, float weights (LE f32; host-only)
    let mut out: Vec<u8> = Vec::new();
    out.extend_from_slice(b"NCM1");
    out.extend_from_slice(&(vocab.len() as u32).to_le_bytes());
    for w in &vocab.words {
        out.extend_from_slice(&(w.len() as u32).to_le_bytes());
        out.extend_from_slice(w.as_bytes());
    }
    let dump = |out: &mut Vec<u8>, v: &Vec<[f32; DIM]>| {
        for a in v {
            for x in a {
                out.extend_from_slice(&x.to_le_bytes());
            }
        }
    };
    dump(&mut out, &model.ctx_emb);
    dump(&mut out, &model.cand_emb);
    for b in &model.bias {
        out.extend_from_slice(&b.to_le_bytes());
    }
    for qrow in &model.qtype_emb {
        for x in qrow {
            out.extend_from_slice(&x.to_le_bytes());
        }
    }
    let mut f = std::fs::File::create(&args[2]).expect("create out");
    f.write_all(&out).expect("write out");
    println!("wrote {} ({} bytes)", args[2], out.len());
}
