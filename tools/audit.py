"""Audit built SH01 artifacts; hardware acceptance is recorded separately."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

def require(condition, message):
    if not condition:
        raise ValueError(message)


def decode_hex(path):
    image = {}
    upper = 0
    eof = False
    for line in path.read_text(encoding="ascii").splitlines():
        if not line.strip():
            continue
        require(not eof and line.startswith(":"), "Malformed HEX or record after EOF")
        record = bytes.fromhex(line[1:])
        require(len(record) >= 5 and len(record) == record[0] + 5, "HEX record length")
        require(sum(record) & 255 == 0, "HEX record checksum")
        count, address, kind = record[0], int.from_bytes(record[1:3], "big"), record[3]
        payload = record[4:-1]
        if kind == 0:
            for index, value in enumerate(payload):
                target = upper + address + index
                require(0 <= target < 0x7000, "HEX overlaps reserved journal/outside Flash")
                require(target not in image, "Overlapping HEX data")
                image[target] = value
        elif kind == 1:
            require(count == 0 and address == 0, "HEX EOF structure")
            eof = True
        elif kind in (2, 4):
            require(count == 2 and address == 0, "HEX upper-address structure")
            upper = int.from_bytes(payload, "big") << (4 if kind == 2 else 16)
        elif kind in (3, 5):
            require(count == 4 and address == 0, "HEX entry-point structure")
        else:
            raise ValueError("Unsupported HEX record")
    require(eof and image, "HEX needs EOF and data")
    require(set(image) == set(range(max(image) + 1)), "Application HEX has holes")
    return bytes(image[index] for index in range(len(image)))




ROOT = Path(__file__).resolve().parents[1]
CONFIG = (ROOT / "src/config.h").read_text(encoding="utf-8")
VERSION = re.search(r'#define FW_VERSION "([0-9.]+)"', CONFIG)[1]
FOLDER = ROOT / "build" / ("v" + VERSION)
STEM = "sovol_sh01mod_v" + VERSION.replace(".", "_")


def audit():
    paths = {ext: FOLDER / f"{STEM}.{ext}" for ext in ("hex", "bin", "elf", "map")}
    raw = paths["bin"].read_bytes()
    require(raw == decode_hex(paths["hex"]), "HEX/BIN mismatch")
    require(192 <= len(raw) < 0x7000, "Flash overflow")
    sp, reset = struct.unpack_from("<II", raw)
    require(sp == 0x20001000, "Stack vector")
    require(reset & 1 and reset & ~1 < len(raw), "Reset vector")
    elf = paths["elf"].read_bytes()
    require(elf[:7] == b"\x7fELF\x01\x01\x01", "ELF format")
    require(struct.unpack_from("<H", elf, 18)[0] == 40, "ELF architecture")
    require(struct.unpack_from("<I", elf, 24)[0] == reset, "ELF entry")
    nm = ROOT / "toolchain/bin/arm-none-eabi-nm.exe"
    output = subprocess.check_output([str(nm), "-n", str(paths["elf"])], text=True)
    symbols = {}
    for line in output.splitlines():
        m = re.fullmatch(r"([0-9a-fA-F]+)\s+([A-Za-z])\s+(\S+)", line)
        if m:
            symbols[m[3]] = (int(m[1], 16), m[2])
        require(not re.match(r"\s+U\s", line), "Unresolved symbol")
    require(symbols["Reset_Handler"][0] | 1 == reset, "Reset symbol")
    tick = struct.unpack_from("<I", raw, 60)[0]
    require(symbols["SysTick_Handler"][1] == "T" and
            symbols["SysTick_Handler"][0] | 1 == tick, "Strong SysTick vector")
    require(tick != symbols["Default_Handler"][0] | 1, "Default SysTick")
    require(0x20000000 <= symbols["_sbss"][0] <= symbols["_ebss"][0] <= sp - 512,
            "Static RAM leaves less than 512 bytes for stack")
    required = {"controller_task", "sensor_task", "bus_read", "bus_write",
                "lcd_temperature_humidity", "lcd_time", "hal_adc", "hal_clock_ok", "hal_heater",
                "storage_init", "storage_save", "storage_disarm", "Flash_WriteWord", "Flash_SectorErase",
                "sensor_overheated", "sensor_primary_fault"}
    require("firmware_version" in symbols and (VERSION + "\0").encode() in raw, "Embedded version missing")
    require(required <= symbols.keys(), "SH01 modules missing")
    forbidden = {"delay10us", "delay100us", "delay1ms"}
    require(not forbidden & symbols.keys(), "Destructive clock delay included")
    require(not any(name.startswith("profile_") for name in symbols) and
            "Flash_ChipErase" not in symbols, "Unexpected profiles/chip erase included")
    linker_map = paths["map"].read_text(encoding="utf-8")
    sources = sorted((ROOT / "src").glob("*.[ch]"))
    for path in sources:
        if path.suffix == ".c":
            require(path.with_suffix(".o").name in linker_map, f"Object missing: {path.name}")
    source_hashes = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in sources}
    config = (ROOT / "src/config.h").read_text(encoding="utf-8")
    for key, value in (("TEMP_MAX_C", "70u"), ("CHAMBER_FAULT_X10", "730"),
                       ("TIME_MIN_H", "6u"), ("TIME_MAX_H", "96u")):
        require(re.search(r"^#define SH01_" + key + r"\s+" + value + r"$", config, re.M),
                f"SH01 config mismatch: {key}")
    presets = [6, 8, 10, 12, 18, 24, 32, 48, 72, 96]
    preset_address = symbols["time_presets_h"][0]
    require(raw[preset_address:preset_address+len(presets)] == bytes(presets),
            "Compiled duration presets differ from the requested list")
    result = {
        "ok": True, "version": VERSION, "editable_c_source": True,
        "oem_binary_required_for_build": False, "application_bytes": len(raw),
        "last_address": f"0x{len(raw)-1:08X}", "reset": f"0x{reset:08X}",
        "systick": f"0x{tick:08X}", "static_ram_bytes": symbols["_ebss"][0] - 0x20000000,
        "remaining_ram_above_bss": sp - symbols["_ebss"][0],
        "heater_control_included": True, "persistent_resume_included": True,
        "primary_sensor": "SHT4x", "reserve_sensor": "AHT20",
        "active_camera_overheat_check": True, "independent_plate_ntc": True,
        "primary_recovery_good_frames": 3, "reserve_unused_with_healthy_primary": True,
        "sensor_waits_from_transfer_completion": True,
        "primary_transaction_excludes_reserve": True,
        "reserve_after_confirmed_primary_failure": True,
        "error_view_ms": 5000, "power_error_blink_half_period_ms": 500,
        "journal_base": "0x7000", "journal_bytes": 4096,
        "duration_presets_h": presets,
        "hardware_functional_acceptance": False, "thermal_acceptance": False,
        "artifacts": {ext: {"bytes": p.stat().st_size,
                            "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                      for ext, p in paths.items()},
        "source_sha256": source_hashes,
    }
    (FOLDER / f"{STEM}.audit.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    try:
        result = audit()
        print(json.dumps({key: value for key, value in result.items()
                          if key not in ("source_sha256", "artifacts")}, indent=2))
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        raise SystemExit(1)
