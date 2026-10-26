"""Offline verification of complete SH01 Flash reads; no serial access/dependencies."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

FLASH = 32768
JOURNAL = 0x7000
PRESETS = [6, 8, 10, 12, 18, 24, 32, 48, 72, 96, 1, 2, 3, 4, 5]  # Stored codes; keep old order.


def require(condition, message):
    if not condition:
        raise ValueError(message)


def load_image(path, full=False):
    path = Path(path)
    raw = path.read_bytes()
    if path.suffix.lower() == '.hex':
        data = {}
        upper = 0
        eof = False
        for number, line in enumerate(raw.decode('ascii').splitlines(), 1):
            if not line.strip():
                continue
            require(not eof and line.startswith(':'), f'HEX syntax/record after EOF: {number}')
            row = bytes.fromhex(line[1:])
            require(len(row) >= 5 and len(row) == row[0] + 5, f'HEX length: {number}')
            require(sum(row) % 256 == 0, f'HEX checksum: {number}')
            count, address, kind = row[0], int.from_bytes(row[1:3], 'big'), row[3]
            payload = row[4:-1]
            if kind == 0:
                for offset, value in enumerate(payload):
                    target = upper + address + offset
                    require(0 <= target < FLASH, 'HEX outside Flash')
                    require(target not in data, 'HEX overlapping data')
                    data[target] = value
            elif kind == 1:
                require(count == 0 and address == 0, 'HEX EOF structure')
                eof = True
            elif kind in (2, 4):
                require(count == 2 and address == 0, 'HEX upper address')
                upper = int.from_bytes(payload, 'big') << (4 if kind == 2 else 16)
            elif kind in (3, 5):
                require(count == 4 and address == 0, 'HEX entry point')
            else:
                raise ValueError('Unsupported HEX record')
        require(eof and data, 'HEX needs data and EOF')
        require(set(data) == set(range(max(data) + 1)), 'Incomplete HEX; padding is forbidden')
        raw = bytes(data[i] for i in range(len(data)))
    else:
        require(path.suffix.lower() == '.bin', 'Use .hex or raw .bin')
    require(len(raw) == FLASH if full else 192 <= len(raw) <= JOURNAL, 'Unexpected image size')
    stack, reset = struct.unpack_from('<II', raw)
    require(0x20000000 < stack <= 0x20001000 and stack % 8 == 0, 'Invalid stack vector')
    require(reset & 1 and reset & ~1 < len(raw), 'Invalid reset vector')
    return raw


def journal_records(raw):
    records = []
    latest = None
    temperatures = list(range(40, 71, 5))
    for page in range(JOURNAL, FLASH, 512):
        for offset in range(0, 500, 20):
            address = page + offset
            magic, sequence, packed, crc, stop = struct.unpack_from('<5I', raw, address)
            if magic != 0x31524853:
                continue  # Empty or uncommitted slot; never invent its payload.
            require(crc == zlib.crc32(raw[address:address + 12]), f'Journal CRC at 0x{address:04X}')
            code, index = (packed >> 19) & 127, (packed >> 26) & 15
            require(index < len(PRESETS), 'Journal duration')
            if code in temperatures:
                temperature, fan = code, 100
            else:
                setting = code - sum(t < code for t in temperatures)
                require(setting < 63, 'Journal temperature/fan code')
                temperature, fan = 40 + setting // 9 * 5, (setting % 9 + 1) * 10
            remaining = packed & 0x7FFFF
            running, powered_off = bool(packed & (1 << 30)), bool(packed & (1 << 31))
            require(remaining <= PRESETS[index] * 3600, 'Journal remaining time')
            require(not running or remaining > 0 and not powered_off, 'Journal RUN/OFF combination')
            record = dict(address=f'0x{address:04X}', sequence=sequence, remaining_s=remaining,
                          target_c=temperature, hours=PRESETS[index], fan_percent=fan,
                          running=running and stop == 0xFFFFFFFF, powered_off=powered_off)
            records.append(record)
            if latest is None or 0 < (sequence - latest['sequence']) % (1 << 32) < (1 << 31):
                latest = record
    return records, latest


def verify(readback, application=None, previous=None, compare_copy=None):
    raw = load_image(readback, full=True)
    result = dict(ok=True, readback_bytes=len(raw), readback_sha256=hashlib.sha256(raw).hexdigest())
    if compare_copy is not None:
        require(application is None and previous is None, 'Copy comparison is a separate mode')
        require(raw == load_image(compare_copy, full=True), 'Complete Flash copies differ')
        result['complete_copies_match'] = True
        return result
    require(application is not None, 'Application HEX/BIN is required')
    app = load_image(application)
    require(raw[:len(app)] == app, 'Application byte mismatch')
    end = (len(app) + 511) // 512 * 512
    require(raw[len(app):end] == b'\xff' * (end - len(app)), 'Erased page padding mismatch')
    result.update(application_bytes=len(app), application_match=True, erased_padding_match=True,
                  application_sha256=hashlib.sha256(app).hexdigest())
    if previous is not None:
        baseline = load_image(previous, full=True)
        require(raw[end:JOURNAL] == baseline[end:JOURNAL], 'Untouched Flash region differs')
        result['unprogrammed_outside_journal_preserved'] = True
        # Journal may change while drying; its CRC is checked instead of assuming equality.
    records, latest = journal_records(raw)
    result.update(journal_valid_records=len(records), latest_cycle=latest)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('readback', type=Path)
    parser.add_argument('--application', type=Path)
    parser.add_argument('--previous', type=Path)
    parser.add_argument('--compare-copy', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    try:
        result = verify(args.readback, args.application, args.previous, args.compare_copy)
        report = json.dumps(result, indent=2) + '\n'
        if args.report:
            inputs = [p.resolve() for p in (args.readback, args.application, args.previous, args.compare_copy) if p]
            require(args.report.resolve() not in inputs, 'Report must not overwrite an input')
            with args.report.open('x', encoding='utf-8') as handle:
                handle.write(report)
        print(report, end='')
        return 0
    except (ValueError, OSError, UnicodeError, struct.error) as error:
        print(json.dumps(dict(ok=False, error=str(error))), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
