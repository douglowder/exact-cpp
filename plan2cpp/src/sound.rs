//! Sounds (`sound`, Exact LLP 1096) and a `sequence` (LLP 1125) in the C++ target. Each
//! declared WAV becomes its samples in flash; the runtime's transport (`rt_transport.cpp`)
//! plays a sequence through the board's speaker. The board runs one gated, unkeyed sequence
//! whose `play` keeps its defaults past `bpm`: no count-in (`from` 0, `countIn` []), no MIDI
//! clock in or out, and no `ended` action. The rest is refused by name.

use std::fmt::Write;
use std::path::Path;

use exact_plan::Plan;

use crate::code::{cpp_string, literal, Literal};

/// Refuses a sequence outside the board's subset.
///
/// # Errors
/// The first argument or form the board does not run, by name.
pub fn check_sequences(plan: &Plan) -> Result<(), String> {
    if plan.sequences.len() > 1 {
        return Err(format!(
            "the app declares {} sequences; the board plays one",
            plan.sequences.len()
        ));
    }
    for q in &plan.sequences {
        let name = plan.str(q.name);
        if !q.gated {
            return Err(format!(
                "sequence {name} has no `when` gate; the board plays a gated one"
            ));
        }
        if q.keyed {
            return Err(format!(
                "sequence {name} is keyed (`key=`); not in the C++ target"
            ));
        }
        if q.ended.is_some() {
            return Err(format!(
                "sequence {name} runs an `ended=` action; not in the C++ target"
            ));
        }
        let defaults = [
            (q.from, Literal::Num(0.0), "`from=` (a count-in)"),
            (q.count_in, Literal::EmptyList, "`countIn=`"),
            (
                q.clock,
                Literal::Str(String::new()),
                "`clock=` (MIDI clock in)",
            ),
            (
                q.clock_out,
                Literal::Str(String::new()),
                "`clockOut=` (MIDI clock out)",
            ),
        ];
        for (body, default, what) in defaults {
            if literal(plan, body)? != Some(default) {
                return Err(format!(
                    "sequence {name} sets {what}; not in the C++ target"
                ));
            }
        }
    }
    Ok(())
}

/// A WAV file's sample rate and 16-bit mono samples; refuses any other format by name.
fn pcm(bytes: &[u8], src: &str) -> Result<(u32, Vec<i16>), String> {
    let u16_at = |i: usize| {
        bytes
            .get(i..i + 2)
            .map(|b| u16::from_le_bytes([b[0], b[1]]))
    };
    let u32_at = |i: usize| {
        bytes
            .get(i..i + 4)
            .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    };
    if bytes.get(0..4) != Some(b"RIFF") || bytes.get(8..12) != Some(b"WAVE") {
        return Err(format!("sound {src} is not a WAV file"));
    }
    let (mut at, mut format, mut data) = (12usize, None, None);
    while let (Some(id), Some(len)) = (bytes.get(at..at + 4), u32_at(at + 4)) {
        let body = at + 8;
        let len = len as usize;
        match id {
            b"fmt " => {
                format = Some((
                    u16_at(body),
                    u16_at(body + 2),
                    u32_at(body + 4),
                    u16_at(body + 14),
                ));
            }
            b"data" => data = bytes.get(body..body + len),
            _ => {}
        }
        at = body + len + (len & 1);
    }
    let Some((Some(kind), Some(channels), Some(rate), Some(bits))) = format else {
        return Err(format!("sound {src} has no readable `fmt ` chunk"));
    };
    if (kind, channels, bits) != (1, 1, 16) {
        return Err(format!(
            "sound {src} is format {kind}, {channels} channels, {bits} bits; the board plays 16-bit mono PCM"
        ));
    }
    let data = data.ok_or_else(|| format!("sound {src} has no `data` chunk"))?;
    let samples = data
        .chunks_exact(2)
        .map(|b| i16::from_le_bytes([b[0], b[1]]))
        .collect();
    Ok((rate, samples))
}

/// Writes each declared sound's samples and `kSounds`; returns how many there are.
///
/// # Errors
/// A sound that cannot be read, is not 16-bit mono PCM, or differs from what the compiler read.
pub fn tables(plan: &Plan, root: &Path, s: &mut String) -> Result<usize, String> {
    let mut rows = Vec::new();
    for (i, row) in plan.sounds.iter().enumerate() {
        let src = plan.str(row.src);
        let path = root.join(src);
        let bytes = std::fs::read(&path).map_err(|e| format!("sound {src}: {e}"))?;
        let (rate, samples) = pcm(&bytes, src)?;
        if rate != row.rate || samples.len() != row.frames as usize {
            return Err(format!(
                "sound {src} changed since the plan was made: rebuild the app"
            ));
        }
        let items: Vec<String> = samples.iter().map(i16::to_string).collect();
        let _ = writeln!(
            s,
            "static const int16_t kSound{i}[] = {{{}}};",
            items.join(", ")
        );
        rows.push(format!(
            "{{{}, kSound{i}, {}, {rate}}}",
            cpp_string(src),
            samples.len()
        ));
    }
    let items = if rows.is_empty() {
        "{nullptr, nullptr, 0, 0}".to_string()
    } else {
        rows.join(", ")
    };
    let _ = writeln!(s, "const rt::Sound kSounds[] = {{{items}}};");
    Ok(rows.len())
}

#[cfg(test)]
mod tests {
    use super::pcm;

    fn wav(kind: u16, channels: u16, bits: u16, data: &[u8]) -> Vec<u8> {
        let mut b = b"RIFF\0\0\0\0WAVEfmt \x10\0\0\0".to_vec();
        for v in [kind, channels] {
            b.extend(v.to_le_bytes());
        }
        b.extend(22050u32.to_le_bytes());
        b.extend((22050u32 * 2).to_le_bytes());
        b.extend(2u16.to_le_bytes());
        b.extend(bits.to_le_bytes());
        b.extend(b"data");
        b.extend(u32::try_from(data.len()).unwrap_or(0).to_le_bytes());
        b.extend(data);
        b
    }

    #[test]
    fn sixteen_bit_mono_pcm_reads_as_its_samples() {
        let (rate, samples) = pcm(&wav(1, 1, 16, &[1, 0, 0xff, 0xff]), "a.wav").unwrap_or_default();
        assert_eq!((rate, samples), (22050, vec![1, -1]));
    }

    #[test]
    fn other_wav_formats_are_refused_by_name() {
        let stereo = pcm(&wav(1, 2, 16, &[]), "a.wav").err().unwrap_or_default();
        assert!(stereo.contains("2 channels"), "{stereo}");
        let float = pcm(&wav(3, 1, 32, &[]), "a.wav").err().unwrap_or_default();
        assert!(float.contains("format 3"), "{float}");
        let text = pcm(b"not a wav", "a.wav").err().unwrap_or_default();
        assert!(text.contains("not a WAV file"), "{text}");
    }
}
