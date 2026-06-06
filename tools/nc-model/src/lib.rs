//! NanoCamelid 32XCD — shared model core.
//!
//! Everything the console must reproduce lives here in exact integer form:
//! tokenizer normalization, n-gram candidate generation/merge, and the
//! quantized reranker forward pass + greedy decode. The trainer uses the
//! float mirror in `train`; the packer serializes; `src/host` (reference
//! runtime) and the console both consume the packed integer model, and the
//! reference's outputs are the source of truth for parity receipts.
//!
//! Integer contract (mirrored by the SH-2 kernels — keep in sync with
//! src/shared/ once M6 lands):
//!   score(c) = (ng << SH_NG)
//!            + (dot_i32(cv, cand_emb[c]) >> SH_DOT)   // arithmetic shift
//!            + (bias[c] << SH_B)
//!            - REP_PEN if c in last REP_WINDOW emitted tokens
//!   cv[d] = sum_{j<CTX} CTX_W[j] * ctx_emb[ctx[j]][d] + QT_W * qtype_emb[qt][d]
//!   EOS forbidden (score = SCORE_MIN) while emitted < MIN_TOKENS.
//!   argmax: strictly-greater scan from token order; first wins (lowest
//!   candidate-slot index on tie).
//! All arithmetic is i32 with wrapping semantics ruled out by construction
//! (|score| bounded well under 2^30; the packer asserts the bound).

pub const VOCAB_MAX: usize = 512;
pub const DIM: usize = 16;
pub const CTX: usize = 4;
pub const K: usize = 16;
pub const TRI_TOP: usize = 8;
pub const BI_TOP: usize = 8;
pub const MAX_TOKENS: usize = 32;
pub const MIN_TOKENS: usize = 4;
pub const REP_WINDOW: usize = 8;
pub const REP_PEN: i32 = 192;
pub const SH_NG: u32 = 4;
pub const QT_COUNT: usize = 12;
/// context weights, most recent last (ctx[CTX-1] is the newest token)
pub const CTX_W: [i32; CTX] = [1, 1, 2, 4];
pub const QT_W: i32 = 2;
pub const SCORE_MIN: i32 = i32::MIN / 4;

pub const PAD: u16 = 0;
pub const UNK: u16 = 1;
pub const EOS: u16 = 2;
pub const SEP: u16 = 3;
pub const N_SPECIAL: usize = 4;

pub const QTYPE_WORDS: [&str; QT_COUNT - 1] = [
    "what", "why", "who", "where", "when", "how", "are", "is", "do", "does", "can",
];

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

/// Normalize raw text into word tokens. Lowercase; alphanumerics and
/// apostrophes form words; ',' and '.' are their own tokens; everything
/// else is whitespace. '?'/'!' never appear as tokens (they delimit the
/// question in line parsing).
pub fn normalize(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut word = String::new();
    for ch in text.chars() {
        let c = ch.to_ascii_lowercase();
        if c.is_ascii_alphanumeric() || c == '\'' {
            word.push(c);
        } else {
            if !word.is_empty() {
                out.push(std::mem::take(&mut word));
            }
            if c == ',' || c == '.' {
                out.push(c.to_string());
            }
        }
    }
    if !word.is_empty() {
        out.push(word);
    }
    out
}

/// Question type from the first question word (index into qtype_emb; 0 = none).
pub fn qtype_of(first_word: Option<&str>) -> usize {
    match first_word {
        Some(w) => QTYPE_WORDS.iter().position(|q| *q == w).map_or(0, |i| i + 1),
        None => 0,
    }
}

#[derive(Clone, Debug)]
pub struct Vocab {
    pub words: Vec<String>, // index = id; ids 0..4 are specials
}

impl Vocab {
    pub fn id(&self, w: &str) -> u16 {
        self.words.iter().position(|x| x == w).map_or(UNK, |i| i as u16)
    }
    pub fn word(&self, id: u16) -> &str {
        self.words.get(id as usize).map_or("<bad>", |s| s.as_str())
    }
    pub fn len(&self) -> usize {
        self.words.len()
    }
    pub fn is_empty(&self) -> bool {
        self.words.is_empty()
    }
}

/// One corpus line: either Q&A ("...? ...") or a plain statement.
#[derive(Clone, Debug)]
pub struct Line {
    pub q: Vec<String>,
    pub a: Vec<String>,
    pub qtype: usize,
}

pub fn parse_line(line: &str) -> Option<Line> {
    let line = line.trim();
    if line.is_empty() {
        return None;
    }
    if let Some(pos) = line.find(['?', '!']) {
        let q = normalize(&line[..pos]);
        let a = normalize(&line[pos + 1..]);
        if q.is_empty() && a.is_empty() {
            return None;
        }
        let qt = qtype_of(q.first().map(|s| s.as_str()));
        Some(Line { q, a, qtype: qt })
    } else {
        let a = normalize(line);
        if a.is_empty() {
            return None;
        }
        Some(Line { q: Vec::new(), a, qtype: 0 })
    }
}

pub fn build_vocab(lines: &[Line]) -> Vocab {
    use std::collections::HashMap;
    let mut counts: HashMap<&str, u32> = HashMap::new();
    for l in lines {
        for w in l.q.iter().chain(l.a.iter()) {
            *counts.entry(w.as_str()).or_default() += 1;
        }
    }
    let mut ranked: Vec<(&str, u32)> = counts.into_iter().collect();
    ranked.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(b.0)));
    let mut words = vec!["<pad>".into(), "<unk>".into(), "<eos>".into(), "<sep>".into()];
    for (w, _) in ranked.into_iter().take(VOCAB_MAX - N_SPECIAL) {
        words.push(w.to_string());
    }
    Vocab { words }
}

/// Token sequence for a line: [q...] SEP [a...] EOS (statements: [a...] EOS).
pub fn line_tokens(v: &Vocab, l: &Line) -> Vec<u16> {
    let mut t = Vec::new();
    if !l.q.is_empty() {
        t.extend(l.q.iter().map(|w| v.id(w)));
        t.push(SEP);
    }
    t.extend(l.a.iter().map(|w| v.id(w)));
    t.push(EOS);
    t
}

// ---------------------------------------------------------------------------
// N-gram candidate tables
// ---------------------------------------------------------------------------

#[derive(Clone, Debug, Default)]
pub struct NgramTables {
    /// sorted by key = (w1 as u32) << 16 | w2; values sorted by (-count, id)
    pub tri: Vec<(u32, Vec<(u16, u32)>)>,
    /// indexed by w2; values sorted by (-count, id)
    pub bi: Vec<Vec<(u16, u32)>>,
    /// global top tokens by count
    pub uni: Vec<(u16, u32)>,
}

pub fn tri_key(w1: u16, w2: u16) -> u32 {
    ((w1 as u32) << 16) | w2 as u32
}

fn ilog2(x: u32) -> u32 {
    31 - x.leading_zeros()
}

/// Deterministic count -> candidate score ladders (u8 range).
pub fn tri_score(count: u32) -> i8 {
    (16 + 8 * ilog2(count)).min(63) as i8
}
pub fn bi_score(count: u32) -> i8 {
    (8 + 4 * ilog2(count)).min(31) as i8
}
pub const UNI_SCORE: i8 = 1;

pub fn build_ngrams(vocab_len: usize, seqs: &[Vec<u16>]) -> NgramTables {
    use std::collections::HashMap;
    let mut tri: HashMap<u32, HashMap<u16, u32>> = HashMap::new();
    let mut bi: Vec<HashMap<u16, u32>> = vec![HashMap::new(); vocab_len];
    let mut uni: HashMap<u16, u32> = HashMap::new();
    for s in seqs {
        for w in &s[..] {
            *uni.entry(*w).or_default() += 1;
        }
        for w in s.windows(2) {
            *bi[w[0] as usize].entry(w[1]).or_default() += 1;
        }
        for w in s.windows(3) {
            *tri.entry(tri_key(w[0], w[1])).or_default().entry(w[2]).or_default() += 1;
        }
    }
    let rank = |m: HashMap<u16, u32>| -> Vec<(u16, u32)> {
        let mut v: Vec<(u16, u32)> = m.into_iter().collect();
        v.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
        v
    };
    let mut tri_v: Vec<(u32, Vec<(u16, u32)>)> =
        tri.into_iter().map(|(k, m)| (k, rank(m))).collect();
    tri_v.sort_by_key(|(k, _)| *k);
    let bi_v: Vec<Vec<(u16, u32)>> = bi.into_iter().map(rank).collect();
    let mut uni_v = rank(uni);
    uni_v.retain(|(t, _)| *t != PAD && *t != SEP);
    uni_v.truncate(K);
    NgramTables { tri: tri_v, bi: bi_v, uni: uni_v }
}

/// Candidate list for context (w1, w2): trigram top-8, then bigram top-8
/// (deduped), then unigram fill, truncated to K. EXACT order matters — the
/// console replicates this merge.
pub fn candidates(t: &NgramTables, w1: u16, w2: u16) -> Vec<(u16, i8)> {
    let mut out: Vec<(u16, i8)> = Vec::with_capacity(K);
    if let Ok(i) = t.tri.binary_search_by_key(&tri_key(w1, w2), |(k, _)| *k) {
        for (tok, c) in t.tri[i].1.iter().take(TRI_TOP) {
            out.push((*tok, tri_score(*c)));
        }
    }
    // Consider exactly the top-BI_TOP ranked bigram entries (matching the
    // packed pool truncation), dedupe against trigram picks, then fill from
    // unigrams. Identical semantics to unpack::candidates_packed().
    let push_unique = |tok: u16, sc: i8, out: &mut Vec<(u16, i8)>| {
        if out.len() < K && !out.iter().any(|(t2, _)| *t2 == tok) {
            out.push((tok, sc));
        }
    };
    for (tok, c) in t.bi.get(w2 as usize).map(|v| &v[..]).unwrap_or(&[]).iter().take(BI_TOP) {
        push_unique(*tok, bi_score(*c), &mut out);
    }
    for (tok, _) in &t.uni {
        if out.len() >= K {
            break;
        }
        push_unique(*tok, UNI_SCORE, &mut out);
    }
    out
}

// ---------------------------------------------------------------------------
// Quantized model + integer inference (the canonical pipeline)
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
pub struct QuantModel {
    pub vocab_len: usize,
    pub ctx_emb: Vec<[i8; DIM]>,   // [vocab]
    pub cand_emb: Vec<[i8; DIM]>,  // [vocab]
    pub bias: Vec<i8>,             // [vocab]
    pub qtype_emb: [[i8; DIM]; QT_COUNT],
    pub sh_dot: u32,
    pub sh_b: u32,
}

/// Context vector from the last CTX tokens (padded with PAD) + qtype.
pub fn context_vector(m: &QuantModel, ctx: &[u16; CTX], qt: usize) -> [i32; DIM] {
    let mut cv = [0i32; DIM];
    for j in 0..CTX {
        let e = &m.ctx_emb[ctx[j] as usize];
        for d in 0..DIM {
            cv[d] += CTX_W[j] * e[d] as i32;
        }
    }
    let qe = &m.qtype_emb[qt];
    for d in 0..DIM {
        cv[d] += QT_W * qe[d] as i32;
    }
    cv
}

pub fn score_candidate(m: &QuantModel, cv: &[i32; DIM], tok: u16, ng: i8,
                       recent: &[u16], emitted: usize) -> i32 {
    if tok == EOS && emitted < MIN_TOKENS {
        return SCORE_MIN;
    }
    if tok == PAD || tok == SEP || tok == UNK {
        return SCORE_MIN; // never emit specials except EOS
    }
    let e = &m.cand_emb[tok as usize];
    let mut dot = 0i32;
    for d in 0..DIM {
        dot += cv[d] * e[d] as i32;
    }
    let mut s = ((ng as i32) << SH_NG) + (dot >> m.sh_dot) + ((m.bias[tok as usize] as i32) << m.sh_b);
    let win = recent.len().min(REP_WINDOW);
    if recent[recent.len() - win..].contains(&tok) {
        s -= REP_PEN;
    }
    s
}

#[derive(Clone, Debug, Default)]
pub struct StepTrace {
    pub ctx: [u16; CTX],
    pub cands: Vec<(u16, i8)>,
    pub scores: Vec<i32>,
    pub chosen: u16,
}

/// Greedy decode. Returns (token ids, per-step traces).
pub fn generate(m: &QuantModel, t: &NgramTables, prompt: &[u16], qt: usize)
                -> (Vec<u16>, Vec<StepTrace>) {
    let mut seq: Vec<u16> = prompt.to_vec();
    let mut out = Vec::new();
    let mut traces = Vec::new();
    for _ in 0..MAX_TOKENS {
        let n = seq.len();
        let w2 = seq[n - 1];
        let w1 = if n >= 2 { seq[n - 2] } else { PAD };
        let mut ctx = [PAD; CTX];
        for j in 0..CTX {
            if n >= CTX - j {
                ctx[j] = seq[n - (CTX - j)];
            }
        }
        let cands = candidates(t, w1, w2);
        let cv = context_vector(m, &ctx, qt);
        let mut best = 0usize;
        let mut best_s = SCORE_MIN;
        let mut scores = Vec::with_capacity(cands.len());
        for (i, (tok, ng)) in cands.iter().enumerate() {
            let s = score_candidate(m, &cv, *tok, *ng, &out, out.len());
            scores.push(s);
            if s > best_s {
                best_s = s;
                best = i;
            }
        }
        // safety: if every candidate is forbidden, fall back to top unigram
        let chosen = if best_s == SCORE_MIN {
            t.uni.first().map_or(UNK, |(tok, _)| *tok)
        } else {
            cands[best].0
        };
        traces.push(StepTrace { ctx, cands, scores, chosen });
        if chosen == EOS {
            break;
        }
        out.push(chosen);
        seq.push(chosen);
    }
    (out, traces)
}

/// Tokenize a user prompt into the generation-ready prefix [q...] SEP.
pub fn prompt_tokens(v: &Vocab, text: &str) -> (Vec<u16>, usize) {
    let words = normalize(&text.replace(['?', '!'], " "));
    let qt = qtype_of(words.first().map(|s| s.as_str()));
    let mut t: Vec<u16> = words.iter().map(|w| v.id(w)).collect();
    if t.is_empty() {
        t.push(UNK);
    }
    t.push(SEP);
    (t, qt)
}

pub fn detok(v: &Vocab, ids: &[u16]) -> String {
    let mut s = String::new();
    for id in ids {
        let w = v.word(*id);
        if w == "," || w == "." {
            s.push_str(w);
        } else {
            if !s.is_empty() {
                s.push(' ');
            }
            s.push_str(w);
        }
    }
    s
}

// ---------------------------------------------------------------------------
// Float training mirror
// ---------------------------------------------------------------------------

pub mod train {
    use super::*;

    /// Deterministic xorshift32 PRNG (seeded; no system entropy).
    pub struct Rng(pub u32);
    impl Rng {
        pub fn next_f(&mut self) -> f32 {
            let mut x = self.0;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            self.0 = x;
            (x as f64 / u32::MAX as f64) as f32
        }
        pub fn uniform(&mut self, lo: f32, hi: f32) -> f32 {
            lo + (hi - lo) * self.next_f()
        }
    }

    #[derive(Clone)]
    pub struct FloatModel {
        pub vocab_len: usize,
        pub ctx_emb: Vec<[f32; DIM]>,
        pub cand_emb: Vec<[f32; DIM]>,
        pub bias: Vec<f32>,
        pub qtype_emb: [[f32; DIM]; QT_COUNT],
    }

    impl FloatModel {
        pub fn init(vocab_len: usize, rng: &mut Rng) -> Self {
            let mut mk = |n: usize, rng: &mut Rng| -> Vec<[f32; DIM]> {
                (0..n)
                    .map(|_| {
                        let mut a = [0f32; DIM];
                        for x in a.iter_mut() {
                            *x = rng.uniform(-0.05, 0.05);
                        }
                        a
                    })
                    .collect()
            };
            let ctx_emb = mk(vocab_len, rng);
            let cand_emb = mk(vocab_len, rng);
            let mut qtype_emb = [[0f32; DIM]; QT_COUNT];
            for q in qtype_emb.iter_mut() {
                for x in q.iter_mut() {
                    *x = rng.uniform(-0.05, 0.05);
                }
            }
            FloatModel { vocab_len, ctx_emb, cand_emb, bias: vec![0.0; vocab_len], qtype_emb }
        }

        pub fn context_vector(&self, ctx: &[u16; CTX], qt: usize) -> [f32; DIM] {
            let mut cv = [0f32; DIM];
            for j in 0..CTX {
                let e = &self.ctx_emb[ctx[j] as usize];
                for d in 0..DIM {
                    cv[d] += CTX_W[j] as f32 * e[d];
                }
            }
            for d in 0..DIM {
                cv[d] += QT_W as f32 * self.qtype_emb[qt][d];
            }
            cv
        }

        /// Float logit in "float score units": ng/16 + dot + bias.
        pub fn logit(&self, cv: &[f32; DIM], tok: u16, ng: i8) -> f32 {
            let e = &self.cand_emb[tok as usize];
            let mut dot = 0f32;
            for d in 0..DIM {
                dot += cv[d] * e[d];
            }
            ng as f32 / 16.0 + dot + self.bias[tok as usize]
        }
    }

    /// One training sample: predict `gold` among `cands` given context.
    pub struct Sample {
        pub ctx: [u16; CTX],
        pub qt: usize,
        pub cands: Vec<(u16, i8)>,
        pub gold_idx: usize,
    }

    /// Build teacher-forced samples from token sequences. Predict positions
    /// in the answer region (after SEP; everything for statements). Samples
    /// where the gold token is not among the candidates are dropped and
    /// counted (coverage is reported by the trainer).
    pub fn build_samples(seqs: &[(Vec<u16>, usize)], tables: &NgramTables)
                         -> (Vec<Sample>, usize, usize) {
        let mut samples = Vec::new();
        let mut total = 0usize;
        let mut missed = 0usize;
        for (seq, qt) in seqs {
            let start = seq.iter().position(|t| *t == SEP).map_or(1, |p| p + 1);
            for pos in start..seq.len() {
                total += 1;
                let w2 = seq[pos - 1];
                let w1 = if pos >= 2 { seq[pos - 2] } else { PAD };
                let cands = candidates(tables, w1, w2);
                let gold = seq[pos];
                match cands.iter().position(|(t, _)| *t == gold) {
                    Some(gi) => {
                        let mut ctx = [PAD; CTX];
                        for j in 0..CTX {
                            if pos >= CTX - j {
                                ctx[j] = seq[pos - (CTX - j)];
                            }
                        }
                        samples.push(Sample { ctx, qt: *qt, cands, gold_idx: gi });
                    }
                    None => missed += 1,
                }
            }
        }
        (samples, total, missed)
    }

    /// One SGD epoch (cross-entropy over the candidate set). Returns mean loss.
    pub fn epoch(m: &mut FloatModel, samples: &[Sample], lr: f32) -> f32 {
        let mut total_loss = 0f64;
        for s in samples {
            let cv = m.context_vector(&s.ctx, s.qt);
            let logits: Vec<f32> =
                s.cands.iter().map(|(t, ng)| m.logit(&cv, *t, *ng)).collect();
            let maxl = logits.iter().cloned().fold(f32::MIN, f32::max);
            let exps: Vec<f32> = logits.iter().map(|l| (l - maxl).exp()).collect();
            let z: f32 = exps.iter().sum();
            let probs: Vec<f32> = exps.iter().map(|e| e / z).collect();
            total_loss += -(probs[s.gold_idx].max(1e-9).ln()) as f64;

            // grad wrt logit_i = p_i - 1[i==gold]
            let mut gcv = [0f32; DIM];
            for (i, (tok, _)) in s.cands.iter().enumerate() {
                let g = probs[i] - if i == s.gold_idx { 1.0 } else { 0.0 };
                let e = m.cand_emb[*tok as usize];
                for d in 0..DIM {
                    gcv[d] += g * e[d];
                    m.cand_emb[*tok as usize][d] -= lr * g * cv[d];
                }
                m.bias[*tok as usize] -= lr * g;
            }
            for j in 0..CTX {
                let t = s.ctx[j] as usize;
                for d in 0..DIM {
                    m.ctx_emb[t][d] -= lr * CTX_W[j] as f32 * gcv[d];
                }
            }
            for d in 0..DIM {
                m.qtype_emb[s.qt][d] -= lr * QT_W as f32 * gcv[d];
            }
        }
        (total_loss / samples.len().max(1) as f64) as f32
    }

    /// Quantize: per-table max-abs scaling to i8, shifts chosen so integer
    /// scores approximate 256 * float score units.
    pub fn quantize(m: &FloatModel) -> QuantModel {
        const S: f32 = 256.0;
        let max_abs_v = |v: &Vec<[f32; DIM]>| -> f32 {
            v.iter().flatten().fold(1e-6f32, |a, x| a.max(x.abs()))
        };
        let ctx_max = max_abs_v(&m.ctx_emb)
            .max(m.qtype_emb.iter().flatten().fold(0f32, |a, x| a.max(x.abs())));
        let cand_max = max_abs_v(&m.cand_emb);
        let bias_max = m.bias.iter().fold(1e-6f32, |a, x| a.max(x.abs()));

        let sa = ctx_max / 127.0;
        let sb = cand_max / 127.0;
        // integer dot = sum(qa*qb); float dot = int_dot * sa*sb
        // want int_dot >> sh_dot ~= S * float_dot  =>  2^-sh = S*sa*sb
        let sh_dot = (-(S * sa * sb).log2()).round().clamp(0.0, 20.0) as u32;
        let sbias = bias_max / 127.0;
        let sh_b = (S * sbias).log2().round().clamp(0.0, 8.0) as u32;

        let q8 = |x: f32, s: f32| -> i8 { (x / s).round().clamp(-127.0, 127.0) as i8 };
        let qv = |v: &Vec<[f32; DIM]>, s: f32| -> Vec<[i8; DIM]> {
            v.iter()
                .map(|a| {
                    let mut o = [0i8; DIM];
                    for d in 0..DIM {
                        o[d] = q8(a[d], s);
                    }
                    o
                })
                .collect()
        };
        let mut qtype_emb = [[0i8; DIM]; QT_COUNT];
        for (qi, q) in m.qtype_emb.iter().enumerate() {
            for d in 0..DIM {
                qtype_emb[qi][d] = q8(q[d], sa);
            }
        }
        QuantModel {
            vocab_len: m.vocab_len,
            ctx_emb: qv(&m.ctx_emb, sa),
            cand_emb: qv(&m.cand_emb, sb),
            bias: m.bias.iter().map(|b| q8(*b, sbias)).collect(),
            qtype_emb,
            sh_dot,
            sh_b,
        }
    }
}

// ---------------------------------------------------------------------------
// Binary serialization (big-endian: console-native)
// ---------------------------------------------------------------------------

pub mod pack {
    use super::*;

    pub struct W(pub Vec<u8>);
    impl W {
        pub fn u8(&mut self, v: u8) {
            self.0.push(v);
        }
        pub fn i8(&mut self, v: i8) {
            self.0.push(v as u8);
        }
        pub fn u16(&mut self, v: u16) {
            self.0.extend_from_slice(&v.to_be_bytes());
        }
        pub fn u32(&mut self, v: u32) {
            self.0.extend_from_slice(&v.to_be_bytes());
        }
    }

    /// vocab.bin: u16 count, then per word u8 len + bytes.
    pub fn vocab_bin(v: &Vocab) -> Vec<u8> {
        let mut w = W(Vec::new());
        w.u16(v.len() as u16);
        for word in &v.words {
            w.u8(word.len() as u8);
            w.0.extend_from_slice(word.as_bytes());
        }
        w.0
    }

    /// tokenizer.bin: qtype word ids (u16 per qtype word, in QTYPE_WORDS
    /// order; 0xFFFF if the word is not in vocab).
    pub fn tokenizer_bin(v: &Vocab) -> Vec<u8> {
        let mut w = W(Vec::new());
        w.u16(QTYPE_WORDS.len() as u16);
        for q in QTYPE_WORDS {
            let id = v.words.iter().position(|x| x == q).map_or(0xFFFF, |i| i as u16);
            w.u16(id);
        }
        w.0
    }

    /// candidates.bin layout (all BE):
    ///   u16 vocab_len, u16 tri_count, u16 uni_count
    ///   tri keys:    tri_count * u32
    ///   tri index:   tri_count * (u16 offset, u8 len) into tri pool
    ///   tri pool:    entries (u16 tok, u8 score), offsets in entries
    ///   bi index:    vocab_len * (u16 offset, u8 len) into bi pool
    ///   bi pool:     entries (u16 tok, u8 score)
    ///   uni:         uni_count * (u16 tok, u8 score)
    /// Pools are truncated to TRI_TOP / BI_TOP at pack time.
    pub fn candidates_bin(vocab_len: usize, t: &NgramTables) -> Vec<u8> {
        let mut w = W(Vec::new());
        w.u16(vocab_len as u16);
        w.u16(t.tri.len() as u16);
        w.u16(t.uni.len() as u16);
        for (k, _) in &t.tri {
            w.u32(*k);
        }
        let mut pool: Vec<(u16, i8)> = Vec::new();
        for (_, v) in &t.tri {
            let off = pool.len();
            let take = v.len().min(TRI_TOP);
            for (tok, c) in v.iter().take(take) {
                pool.push((*tok, tri_score(*c)));
            }
            w.u16(off as u16);
            w.u8(take as u8);
        }
        for (tok, sc) in &pool {
            w.u16(*tok);
            w.i8(*sc);
        }
        let mut bpool: Vec<(u16, i8)> = Vec::new();
        let mut bidx: Vec<(u16, u8)> = Vec::new();
        for v in &t.bi {
            let off = bpool.len();
            let take = v.len().min(BI_TOP);
            for (tok, c) in v.iter().take(take) {
                bpool.push((*tok, bi_score(*c)));
            }
            bidx.push((off as u16, take as u8));
        }
        for (off, len) in &bidx {
            w.u16(*off);
            w.u8(*len);
        }
        for (tok, sc) in &bpool {
            w.u16(*tok);
            w.i8(*sc);
        }
        for (tok, c) in &t.uni {
            w.u16(*tok);
            w.i8(if *c > 0 { UNI_SCORE } else { 0 });
        }
        w.0
    }

    /// weights.bin: u16 vocab_len, u8 dim, u8 qt_count, u8 sh_dot, u8 sh_b,
    /// then ctx_emb, cand_emb (vocab*dim i8 each), bias (vocab i8),
    /// qtype_emb (qt_count*dim i8).
    pub fn weights_bin(m: &QuantModel) -> Vec<u8> {
        let mut w = W(Vec::new());
        w.u16(m.vocab_len as u16);
        w.u8(DIM as u8);
        w.u8(QT_COUNT as u8);
        w.u8(m.sh_dot as u8);
        w.u8(m.sh_b as u8);
        for e in &m.ctx_emb {
            for d in 0..DIM {
                w.i8(e[d]);
            }
        }
        for e in &m.cand_emb {
            for d in 0..DIM {
                w.i8(e[d]);
            }
        }
        for b in &m.bias {
            w.i8(*b);
        }
        for q in &m.qtype_emb {
            for d in 0..DIM {
                w.i8(q[d]);
            }
        }
        w.0
    }

    /// decode.bin: decode-loop constants the console must honor.
    pub fn decode_bin() -> Vec<u8> {
        let mut w = W(Vec::new());
        w.u8(K as u8);
        w.u8(CTX as u8);
        w.u8(MAX_TOKENS as u8);
        w.u8(MIN_TOKENS as u8);
        w.u8(REP_WINDOW as u8);
        w.u8(SH_NG as u8);
        w.u16(REP_PEN as u16);
        for cw in CTX_W {
            w.u8(cw as u8);
        }
        w.u8(QT_W as u8);
        w.0
    }
}

// ---------------------------------------------------------------------------
// Binary deserialization (the reference runtime loads ONLY packed files)
// ---------------------------------------------------------------------------

pub mod unpack {
    use super::*;

    pub struct R<'a>(pub &'a [u8], pub usize);
    impl<'a> R<'a> {
        pub fn u8(&mut self) -> u8 {
            let v = self.0[self.1];
            self.1 += 1;
            v
        }
        pub fn i8(&mut self) -> i8 {
            self.u8() as i8
        }
        pub fn u16(&mut self) -> u16 {
            u16::from_be_bytes([self.u8(), self.u8()])
        }
        pub fn u32(&mut self) -> u32 {
            u32::from_be_bytes([self.u8(), self.u8(), self.u8(), self.u8()])
        }
    }

    pub fn vocab(buf: &[u8]) -> Vocab {
        let mut r = R(buf, 0);
        let n = r.u16() as usize;
        let mut words = Vec::with_capacity(n);
        for _ in 0..n {
            let l = r.u8() as usize;
            words.push(String::from_utf8(buf[r.1..r.1 + l].to_vec()).unwrap());
            r.1 += l;
        }
        Vocab { words }
    }

    pub fn candidates(buf: &[u8]) -> NgramTables {
        let mut r = R(buf, 0);
        let vocab_len = r.u16() as usize;
        let tri_count = r.u16() as usize;
        let uni_count = r.u16() as usize;
        let keys: Vec<u32> = (0..tri_count).map(|_| r.u32()).collect();
        let tri_idx: Vec<(u16, u8)> = (0..tri_count).map(|_| (r.u16(), r.u8())).collect();
        let tri_pool_len: usize = tri_idx.iter().map(|(_, l)| *l as usize).sum();
        let tri_pool: Vec<(u16, i8)> =
            (0..tri_pool_len).map(|_| (r.u16(), r.i8())).collect();
        let bi_idx: Vec<(u16, u8)> = (0..vocab_len).map(|_| (r.u16(), r.u8())).collect();
        let bi_pool_len: usize = bi_idx.iter().map(|(_, l)| *l as usize).sum();
        let bi_pool: Vec<(u16, i8)> = (0..bi_pool_len).map(|_| (r.u16(), r.i8())).collect();
        let uni: Vec<(u16, i8)> = (0..uni_count).map(|_| (r.u16(), r.i8())).collect();

        // Reconstruct NgramTables with scores already laddered: store score
        // in the count slot so candidates() reproduces them via *_score?
        // No — keep packed semantics: scores are final. We rebuild tables
        // where "count" is replaced by a value that maps back through the
        // ladder identically. To avoid that fragility, unpacked tables carry
        // the FINAL scores and candidates() must not re-ladder. We signal
        // this by storing score directly and using the packed-table merge
        // below instead.
        let tri = keys
            .iter()
            .zip(tri_idx.iter())
            .map(|(k, (off, len))| {
                let v = tri_pool[*off as usize..*off as usize + *len as usize]
                    .iter()
                    .map(|(t, s)| (*t, *s as u32))
                    .collect();
                (*k, v)
            })
            .collect();
        let bi = bi_idx
            .iter()
            .map(|(off, len)| {
                bi_pool[*off as usize..*off as usize + *len as usize]
                    .iter()
                    .map(|(t, s)| (*t, *s as u32))
                    .collect()
            })
            .collect();
        let uni = uni.iter().map(|(t, s)| (*t, *s as u32)).collect();
        NgramTables { tri, bi, uni }
    }

    /// Candidate merge over UNPACKED tables (scores final, no ladder).
    /// Must match pack-side candidate semantics exactly.
    pub fn candidates_packed(t: &NgramTables, w1: u16, w2: u16) -> Vec<(u16, i8)> {
        let mut out: Vec<(u16, i8)> = Vec::with_capacity(K);
        if let Ok(i) = t.tri.binary_search_by_key(&tri_key(w1, w2), |(k, _)| *k) {
            for (tok, s) in t.tri[i].1.iter().take(TRI_TOP) {
                out.push((*tok, *s as i8));
            }
        }
        let push_unique = |tok: u16, sc: i8, out: &mut Vec<(u16, i8)>| {
            if out.len() < K && !out.iter().any(|(t2, _)| *t2 == tok) {
                out.push((tok, sc));
            }
        };
        for (tok, s) in t.bi.get(w2 as usize).map(|v| &v[..]).unwrap_or(&[]).iter().take(BI_TOP) {
            push_unique(*tok, *s as i8, &mut out);
        }
        for (tok, s) in &t.uni {
            if out.len() >= K {
                break;
            }
            push_unique(*tok, *s as i8, &mut out);
        }
        out
    }

    pub fn weights(buf: &[u8]) -> QuantModel {
        let mut r = R(buf, 0);
        let vocab_len = r.u16() as usize;
        let dim = r.u8() as usize;
        let qt = r.u8() as usize;
        assert_eq!(dim, DIM, "weights.bin dim mismatch");
        assert_eq!(qt, QT_COUNT, "weights.bin qtype count mismatch");
        let sh_dot = r.u8() as u32;
        let sh_b = r.u8() as u32;
        let mut emb = |r: &mut R| -> Vec<[i8; DIM]> {
            (0..vocab_len)
                .map(|_| {
                    let mut a = [0i8; DIM];
                    for d in a.iter_mut() {
                        *d = r.i8();
                    }
                    a
                })
                .collect()
        };
        let ctx_emb = emb(&mut r);
        let cand_emb = emb(&mut r);
        let bias: Vec<i8> = (0..vocab_len).map(|_| r.i8()).collect();
        let mut qtype_emb = [[0i8; DIM]; QT_COUNT];
        for q in qtype_emb.iter_mut() {
            for d in q.iter_mut() {
                *d = r.i8();
            }
        }
        QuantModel { vocab_len, ctx_emb, cand_emb, bias, qtype_emb, sh_dot, sh_b }
    }
}

/// Greedy decode over PACKED tables (final scores; the reference path).
pub fn generate_packed(m: &QuantModel, t: &NgramTables, prompt: &[u16], qt: usize)
                       -> (Vec<u16>, Vec<StepTrace>) {
    let mut seq: Vec<u16> = prompt.to_vec();
    let mut out = Vec::new();
    let mut traces = Vec::new();
    for _ in 0..MAX_TOKENS {
        let n = seq.len();
        let w2 = seq[n - 1];
        let w1 = if n >= 2 { seq[n - 2] } else { PAD };
        let mut ctx = [PAD; CTX];
        for j in 0..CTX {
            if n >= CTX - j {
                ctx[j] = seq[n - (CTX - j)];
            }
        }
        let cands = unpack::candidates_packed(t, w1, w2);
        let cv = context_vector(m, &ctx, qt);
        let mut best = 0usize;
        let mut best_s = SCORE_MIN;
        let mut scores = Vec::with_capacity(cands.len());
        for (i, (tok, ng)) in cands.iter().enumerate() {
            let s = score_candidate(m, &cv, *tok, *ng, &out, out.len());
            scores.push(s);
            if s > best_s {
                best_s = s;
                best = i;
            }
        }
        let chosen = if best_s == SCORE_MIN {
            t.uni.first().map_or(UNK, |(tok, _)| *tok)
        } else {
            cands[best].0
        };
        traces.push(StepTrace { ctx, cands, scores, chosen });
        if chosen == EOS {
            break;
        }
        out.push(chosen);
        seq.push(chosen);
    }
    (out, traces)
}
