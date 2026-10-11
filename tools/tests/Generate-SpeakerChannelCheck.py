"""Create sequential PCM speaker checks, without playing or changing devices.

Windows WAVE channel-mask order is intentional: back precedes side.
The 12-channel file is ordinary PCM, not an Atmos bitstream.
"""
from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import wave

RATE = 48000
SLOT_SECONDS = 6
LEAD_SECONDS = 2
PEAK = 10 ** (-24 / 20)
TONE_PEAK = 10 ** (-30 / 20)
CHANNELS = [
    ('FL', 'Front left', '左前', 0x1),
    ('FR', 'Front right', '右前', 0x2),
    ('FC', 'Center', '中置', 0x4),
    ('LFE', 'Low frequency effects', '低频效果', 0x8),
    ('BL', 'Back left', '左后', 0x10),
    ('BR', 'Back right', '右后', 0x20),
    ('SL', 'Side left', '左侧环绕', 0x200),
    ('SR', 'Side right', '右侧环绕', 0x400),
    ('TFL', 'Top front left', '左前顶部', 0x1000),
    ('TFR', 'Top front right', '右前顶部', 0x4000),
    ('TBL', 'Top back left', '左后顶部', 0x8000),
    ('TBR', 'Top back right', '右后顶部', 0x20000),
]


def chunk(tag: bytes, payload: bytes) -> bytes:
    return tag + struct.pack('<I', len(payload)) + payload + b'\0' * (len(payload) % 2)


def read_voice(path: Path) -> array.array:
    with wave.open(str(path), 'rb') as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (1, 2, RATE):
            raise ValueError(f'Expected mono PCM16/48kHz voice: {path}')
        voice = array.array('h', source.readframes(source.getnframes()))
    if sys.byteorder != 'little':
        voice.byteswap()
    if len(voice) > int(3.5 * RATE):
        raise ValueError(f'Voice does not fit slot: {path}')
    peak = max((abs(value) for value in voice), default=0)
    if not peak:
        raise ValueError(f'Empty voice: {path}')
    factor = 32767 * PEAK / peak
    return array.array('h', (round(value * factor) for value in voice))


def tone(frequency: float, seconds: float) -> array.array:
    count = round(seconds * RATE)
    fade = round(0.02 * RATE)
    return array.array('h', (
        round(32767 * TONE_PEAK * min(1, i / fade, (count - 1 - i) / fade)
              * math.sin(2 * math.pi * frequency * i / RATE))
        for i in range(count)
    ))


def create_file(output: Path, voices: Path, channel_count: int) -> dict:
    selected = CHANNELS[:channel_count]
    total_seconds = LEAD_SECONDS * 2 + SLOT_SECONDS * channel_count
    total_frames = RATE * total_seconds
    channel_mask = sum(row[3] for row in selected)
    if output.exists():
        raise FileExistsError(output)
    samples = array.array('h', [0]) * (total_frames * channel_count)
    schedule = []
    cue_records = []
    cue_labels = b''
    for index, (key, name, chinese, _) in enumerate(selected):
        start = LEAD_SECONDS + index * SLOT_SECONDS
        events = [(4.3, tone(60 if key == 'LFE' else 440, 1))]
        if key != 'LFE':
            events.insert(0, (0.25, read_voice(voices / (key + '.wav'))))
        else:
            # LFE identification stays bass-only, with two separate 60Hz pulses.
            events.insert(0, (0.25, tone(60, 1)))
        for offset, signal in events:
            frame = round((start + offset) * RATE)
            samples[frame * channel_count + index:
                    (frame + len(signal)) * channel_count + index:channel_count] = signal
        cue_id = index + 1
        cue_frame = round((start + 0.25) * RATE)
        cue_records.append(struct.pack('<II4sIII', cue_id, cue_frame, b'data', 0, 0, cue_frame))
        cue_labels += chunk(b'labl', struct.pack('<I', cue_id) + f'{cue_id:02} {key} {name}'.encode('ascii') + b'\0')
        schedule.append({'channel': cue_id, 'role': key, 'name': name,
                         'name_zh': chinese, 'slot_start_seconds': start,
                         'signal_start_seconds': start + 0.25,
                         'slot_end_seconds': start + SLOT_SECONDS})
    per_channel = []
    for index in range(channel_count):
        values = samples[index::channel_count]
        nonzero_frames = [i for i, value in enumerate(values) if value]
        lower = int(schedule[index]['slot_start_seconds'] * RATE)
        upper = int(schedule[index]['slot_end_seconds'] * RATE)
        if not nonzero_frames or nonzero_frames[0] < lower or nonzero_frames[-1] >= upper:
            raise AssertionError(f'Channel isolation failed: {index + 1}')
        peak = max(abs(value) for value in values)
        if peak > math.ceil(32767 * PEAK):
            raise AssertionError('Peak exceeded target')
        per_channel.append({'channel': index + 1, 'peak_dbfs': round(20 * math.log10(peak / 32768), 3),
                            'first_nonzero_frame': nonzero_frames[0], 'last_nonzero_frame': nonzero_frames[-1],
                            'outside_own_slot_nonzero_frames': 0})
    fmt = struct.pack('<HHIIHHHHI', 0xFFFE, channel_count, RATE,
                      RATE * channel_count * 2, channel_count * 2, 16, 22, 16, channel_mask)
    fmt += bytes.fromhex('0100000000001000800000aa00389b71')
    if sys.byteorder != 'little':
        samples.byteswap()
    comment = b'SE sequential speaker check; PCM, not Dolby Atmos; voice peak -24dBFS, tones -30dBFS.\0'
    body = b'WAVE' + chunk(b'fmt ', fmt)
    body += chunk(b'cue ', struct.pack('<I', channel_count) + b''.join(cue_records))
    body += chunk(b'LIST', b'adtl' + cue_labels)
    body += chunk(b'LIST', b'INFO' + chunk(b'ICMT', comment))
    body += chunk(b'data', samples.tobytes())
    output.write_bytes(b'RIFF' + struct.pack('<I', len(body)) + body)
    return {'file': output.name, 'sample_rate': RATE, 'channels': channel_count,
            'bits_per_sample': 16, 'channel_mask': f'0x{channel_mask:X}',
            'frames': total_frames, 'duration_seconds': total_seconds,
            'bytes': output.stat().st_size, 'sha256': hashlib.file_digest(output.open('rb'), 'sha256').hexdigest(),
            'schedule': schedule, 'verification': per_channel}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--voice-directory', type=Path, required=True)
    parser.add_argument('--output-directory', type=Path, required=True)
    args = parser.parse_args()
    args.output_directory.mkdir(parents=True, exist_ok=True)
    records = [create_file(args.output_directory / f'SE-speaker-check-{layout}-48k.wav',
                           args.voice_directory, count)
               for layout, count in [('stereo', 2), ('7.1', 8), ('7.1.4', 12)]]
    manifest = {'schema': 'singleeditor.speaker-check.v1', 'not_atmos_encoded': True,
                'generator': Path(__file__).name, 'files': records}
    destination = args.output_directory / 'speaker-check-manifest.json'
    if destination.exists():
        raise FileExistsError(destination)
    destination.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'generated': [{'file': x['file'], 'channels': x['channels'],
                                   'seconds': x['duration_seconds'], 'sha256': x['sha256']} for x in records]}, indent=2))


if __name__ == '__main__':
    main()
