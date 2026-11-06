"""Export the sounds in Xbox 360 XACT wavebanks (.xwb, big-endian, version 23
as used by Perfect Dark Zero) to WAV, named by bank and cue.

usage: python -I tools/xwb_export.py OUT_DIR AUDIO_DIR [AUDIO_DIR ...] [--banks name,name]

For each <bank>.xwb (all in AUDIO_DIR, or only --banks) it reads the
matching <bank>.xsb for cue names and writes <bank>_<cue>.wav (or
<bank>_wave<N>.wav for a wave no cue uses) plus index.txt with length,
channels, rate and a rough character guess. Read-only on the inputs.

Formats seen: entry format word bit 0 = tag (0 PCM), bits 1-3 channels,
bits 4-21 sample rate, bit 31 16-bit; PCM data is big-endian.
"""
import math
import os
import struct
import sys
import wave


def be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def be16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def read_xwb(path):
    d = open(path, 'rb').read()
    if d[:4] != b'DNBW':
        raise ValueError('%s: not a big-endian wavebank' % path)
    version = be32(d, 4)
    seg = [(be32(d, 8 + 8 * i), be32(d, 12 + 8 * i)) for i in range(5)]
    bank_off = seg[0][0]
    count = be32(d, bank_off + 4)
    name = d[bank_off + 8:bank_off + 72].split(b'\0')[0].decode()
    meta_size = be32(d, bank_off + 72)
    meta_off, data_off = seg[1][0], seg[4][0]
    entries = []
    for i in range(count):
        o = meta_off + i * meta_size
        flags_duration, fmt, play_off, play_len, loop_start, loop_len = struct.unpack_from('>6I', d, o)
        entries.append({
            'tag': fmt & 1,
            'channels': (fmt >> 1) & 7,
            'rate': (fmt >> 4) & 0x3FFFF,
            'bits': 16 if fmt >> 31 else 8,
            'frames': flags_duration >> 4,
            'data': d[data_off + play_off:data_off + play_off + play_len],
            'loop': (loop_start, loop_len),
        })
    return version, name, entries


def read_xsb_cues(path):
    """{wave index: cue name} from a soundbank with simple cues."""
    if not os.path.exists(path):
        return {}
    d = open(path, 'rb').read()
    if d[:4] != b'KBDS':
        return {}
    cues = d[0x0A]                 # simple cues (the u16 at 0x12 counts sounds)
    cue_table = be32(d, 0x1A)      # 5 bytes per cue: flags, sound offset
    names_at = be32(d, 0x22)       # cue names, null-terminated, in cue order
    names = [s.decode('latin1') for s in d[names_at:].split(bytes(1))[:cues]]
    result = {}
    for i in range(cues):
        # Simple sounds only (one track, one wave); others keep wave numbers.
        try:
            sound = be32(d, cue_table + 5 * i + 1)
            track = be32(d, sound + 0x12)
            wave_index = be16(d, track + 8)
        except struct.error:
            continue
        if i < len(names) and names[i]:
            result.setdefault(wave_index, names[i])
    return result


def describe(samples, channels, rate):
    """Rough character guess from the signal: (rms dB, onset periodicity, label)."""
    frames = len(samples) // channels
    if frames < rate:
        return -99.0, 0.0, 'stinger (short)'
    mono = [sum(samples[f * channels + c] for c in range(channels)) / channels for f in range(0, frames)]
    rms = math.sqrt(sum(x * x for x in mono) / len(mono)) or 1e-9
    db = 20 * math.log10(rms / 32768)
    # 50 ms energy envelope and its autocorrelation at 0.25-2 s lags (beat-like structure).
    hop = rate // 20
    env = [math.sqrt(sum(x * x for x in mono[i:i + hop]) / hop) for i in range(0, len(mono) - hop, hop)]
    flux = [max(0.0, env[i] - env[i - 1]) for i in range(1, len(env))]
    mean = sum(flux) / len(flux)
    flux = [x - mean for x in flux]
    var = sum(x * x for x in flux) or 1e-9
    best = max(sum(flux[i] * flux[i + lag] for i in range(len(flux) - lag)) / var for lag in range(5, 41))
    label = 'music (rhythmic)' if best > 0.3 else 'ambience (no steady beat)'
    return db, best, label


def write_wav(path, e):
    samples = struct.unpack('>%dh' % (len(e['data']) // 2), e['data'][:len(e['data']) // 2 * 2])
    with wave.open(path, 'wb') as w:
        w.setnchannels(e['channels'])
        w.setsampwidth(2)
        w.setframerate(e['rate'])
        w.writeframes(struct.pack('<%dh' % len(samples), *samples))
    return samples


def main():
    args = sys.argv[1:]
    banks = None
    if '--banks' in args:
        i = args.index('--banks')
        banks = [b.lower() for b in args[i + 1].split(',')]
        del args[i:i + 2]
    out, dirs = args[0], args[1:]
    os.makedirs(out, exist_ok=True)
    lines = []
    for folder in dirs:
        for f in sorted(os.listdir(folder)):
            if not f.lower().endswith('.xwb'):
                continue
            bank = f[:-4].lower()
            if banks and bank not in banks:
                continue
            version, name, entries = read_xwb(os.path.join(folder, f))
            cues = read_xsb_cues(os.path.join(folder, f[:-4] + '.xsb'))
            for i, e in enumerate(entries):
                cue = cues.get(i)
                target = '%s_%s.wav' % (bank, cue if cue else 'wave%d' % i)
                if e['tag'] != 0 or e['bits'] != 16:
                    lines.append('%s\tskipped: format tag %d, %d bits' % (target, e['tag'], e['bits']))
                    continue
                samples = write_wav(os.path.join(out, target), e)
                seconds = len(samples) / e['channels'] / e['rate']
                db, beat, label = describe(samples, e['channels'], e['rate'])
                lines.append('%s\tcue %s\t%.1f s\t%d ch\t%d Hz\t%.1f dBFS rms\tbeat %.2f\t%s%s' % (
                    target, cue or '(none)', seconds, e['channels'], e['rate'], db, beat, label,
                    '\tloop %d+%d bytes' % e['loop'] if e['loop'][1] else ''))
                print(lines[-1])
    with open(os.path.join(out, 'index.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')


main()
