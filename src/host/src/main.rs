//! nc-reference — the source-of-truth inference runtime (Milestone 5).
//!
//! Loads ONLY the packed console model files (model/*.bin, big-endian) and
//! runs the exact integer pipeline. The console implementation must agree
//! with this program token-for-token; receipts compare against its output.
//!
//! Usage:
//!   nc-reference MODELDIR --prompt "why do llamas hum"
//!   nc-reference MODELDIR --eval QUESTIONS.TXT OUT.json OUT.bin

use nc_model::{unpack, *};

fn esc(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: nc-reference MODELDIR --prompt TEXT | --eval IN.TXT OUT.json OUT.bin");
        std::process::exit(2);
    }
    let dir = &args[1];
    let rd = |name: &str| std::fs::read(format!("{dir}/{name}")).expect(name);
    let vocab = unpack::vocab(&rd("vocab.bin"));
    let tables = unpack::candidates(&rd("candidates.bin"));
    let model = unpack::weights(&rd("weights.bin"));

    let run = |text: &str| -> (Vec<u16>, Vec<u16>, String, usize, u8) {
        let (prompt, qt) = prompt_tokens(&vocab, text);
        let (ids, _traces, fb) = generate_packed(&model, &tables, &prompt, qt);
        let decoded = detok(&vocab, &ids);
        (prompt, ids, decoded, qt, fb)
    };

    match args[2].as_str() {
        "--prompt" => {
            let text = args.get(3).expect("prompt text");
            let (prompt, ids, decoded, qt, fb) = run(text);
            println!("normalized: {:?}", normalize(&text.replace(['?', '!'], " ")));
            println!("qtype: {qt}");
            println!("prompt ids: {prompt:?}");
            println!("output ids: {ids:?}");
            println!("fallback: {fb}");
            println!("decoded: {decoded}");
        }
        "--eval" => {
            let infile = args.get(3).expect("eval file");
            let out_json = args.get(4).expect("out json");
            let out_bin = args.get(5).expect("out bin");
            let questions = std::fs::read_to_string(infile).expect("read eval");
            let mut json = String::from("{\n  \"outputs\": [\n");
            let mut bin: Vec<u8> = Vec::new();
            let qs: Vec<&str> =
                questions.lines().map(str::trim).filter(|l| !l.is_empty()).collect();
            bin.extend_from_slice(&(qs.len() as u16).to_be_bytes());
            for (i, qtext) in qs.iter().enumerate() {
                let (prompt, ids, decoded, qt, fb) = run(qtext);
                let norm = normalize(&qtext.replace(['?', '!'], " ")).join(" ");
                json.push_str(&format!(
                    "    {{ \"input\": \"{}\", \"normalized\": \"{}\", \"qtype\": {qt}, \
                     \"fallback\": {fb}, \"prompt_ids\": {:?}, \"output_ids\": {:?}, \
                     \"decoded\": \"{}\" }}{}\n",
                    esc(qtext), esc(&norm), prompt, ids, esc(&decoded),
                    if i + 1 < qs.len() { "," } else { "" }
                ));
                bin.extend_from_slice(&(prompt.len() as u16).to_be_bytes());
                for t in &prompt {
                    bin.extend_from_slice(&t.to_be_bytes());
                }
                bin.extend_from_slice(&(ids.len() as u16).to_be_bytes());
                for t in &ids {
                    bin.extend_from_slice(&t.to_be_bytes());
                }
                bin.extend_from_slice(&(fb as u16).to_be_bytes());
            }
            json.push_str("  ]\n}\n");
            std::fs::write(out_json, json).expect("write json");
            std::fs::write(out_bin, bin).expect("write bin");
            println!("wrote {} prompts to {out_json} / {out_bin}", qs.len());
        }
        other => {
            eprintln!("unknown mode {other}");
            std::process::exit(2);
        }
    }
}
