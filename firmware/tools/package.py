from pathlib import Path
import json, hashlib, struct, shutil

ROOT = Path(__file__).resolve().parents[1]
out = ROOT / "build/releases"
out.mkdir(parents=True, exist_ok=True)
fields = json.loads((ROOT / "ec/settings_map.json").read_text())["ec_v25"][
    "all_objects"
]
base = (ROOT / "build/ec_v25/EC_v25_candidate.bin").read_bytes()


def uf2(b, start=0x4000):
    aa = list(range(start, 0x80000, 256))
    return b"".join(
        struct.pack(
            "<8I", 0x0A324655, 0x9E5D5157, 0x2000, a, 256, i, len(aa), 0x55114460
        )
        + b[a : a + 256]
        + bytes(220)
        + struct.pack("<I", 0x0AB16F30)
        for i, a in enumerate(aa)
    )


# Fields absent from v21 keep their v25 baseline values.
mapping = [
    ("ir_rate", 0x4C000, 0x44000, 4),
    ("initialized", 0x3C000, 0x46000, 1),
    ("telemetry_blackbox_mode", 0x24000, 0x20000, 1),
    ("poll_fc_arm", 0x36000, 0x42000, 1),
    ("system_channel", 0x38000, 0x36000, 1),
    ("ir_disabled", 0x46000, 0x4A000, 1),
    ("osd_disabled", 0x26000, 0x2E000, 1),
    ("telemetry_disabled", 0x28000, 0x40000, 1),
    ("poll_color", 0x3A000, 0x24000, 1),
    ("osd_rate", 0x58000, 0x38000, 4),
    ("font_initialized", 0x5A000, 0x22000, 1),
    ("osd_debug", 0x40000, 0x32000, 1),
    ("osd_volts", 0x56000, 0x3E000, 1),
    ("osd_voltage_offset", 0x2C000, 0x2C000, 4),
    ("diagnostics_override", 0x4A000, 0x3A000, 1),
    ("telemetry_rate", 0x44000, 0x2A000, 4),
    ("telemetry_while_disarmed", 0x2A000, 0x26000, 1),
    ("telemetry_use_fc_values", 0x54000, 0x28000, 1),
    ("camera_type", 0x4E000, 0x48000, 1),
    ("filesystem_initialized", 0x50000, 0x30000, 1),
]
for target, source in [("F7", "ec_v23.bin"), ("F4", "ec_v21.bin")]:
    previous = (ROOT / "baselines" / source).read_bytes()
    b = bytearray(base)
    b[:0x4000] = previous[:0x4000]
    migrations = []
    if target == "F7":
        for f in fields.values():
            a = int(f["flash"], 16)
            n = f["size"]
            b[a : a + n] = previous[a : a + n]
            migrations.append({"source": hex(a), "destination": hex(a), "size": n})
    else:
        for name, a, d, n in mapping:
            b[d : d + n] = previous[a : a + n]
            migrations.append(
                {"field": name, "source": hex(a), "destination": hex(d), "size": n}
            )
    (out / f"EC_v25_{target}_candidate.bin").write_bytes(b)
    (out / f"EC_v25_{target}_application.uf2").write_bytes(uf2(b))
    (out / f"EC_v25_{target}_migration.json").write_text(
        json.dumps(
            {
                "state": "OFFLINE_CANDIDATE_NOT_HARDWARE_VALIDATED",
                "settings_source": source,
                "migration": migrations,
                "new_v25_fields_keep_baseline": target == "F4",
                "bootloader_excluded_from_uf2": True,
            },
            indent=2,
        )
    )
for target in ("RACER4S", "RACER4F4"):
    shutil.copy2(
        ROOT / f"build/betaflight/obj/betaflight_3.5.7_{target}.hex",
        out / f"FC_{target}.hex",
    )
b = bytearray((ROOT / "build/ledc_v22/LEDC_v22_candidate.bin").read_bytes())
(out / "LEDC_v22_AB_application.uf2").write_bytes(uf2(b))
(out / "LEDC_v22_AB_candidate.bin").write_bytes(b)
# C boards use the same protocol/application but different physical pin assignments.
for a, old, new in [
    (0x774A, 3, 19),
    (0x777A, 6, 18),
    (0x7792, 17, 16),
    (0x77A8, 11, 10),
    (0x5B7E, 16, 17),
    (0x5BA8, 16, 17),
    (0x5BFA, 16, 17),
    (0x5C0A, 16, 17),
    (0xA2FA, 10, 33),
    (0xB042, 10, 33),
    (0xA304, 18, 6),
    (0xA322, 18, 6),
    (0xB018, 18, 6),
    (0xB034, 18, 6),
]:
    assert b[a] == old, (hex(a), b[a], old)
    b[a] = new
assert b[0xB024:0xB028] == bytes.fromhex("4ff01200")
b[0xB024:0xB028] = bytes.fromhex("4ff00600")
(out / "LEDC_v22_C_application.uf2").write_bytes(uf2(b))
(out / "LEDC_v22_C_candidate.bin").write_bytes(b)
manifest = {
    "state": "OFFLINE_CANDIDATES",
    "hardware_flashed": False,
    "files": {
        p.name: {
            "size": p.stat().st_size,
            "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
        }
        for p in out.iterdir()
        if p.suffix in (".bin", ".uf2", ".hex")
    },
}
(out / "manifest.json").write_text(json.dumps(manifest, indent=2))
print("Packaged F4/F7 FC and EC candidates; separate LED board wiring variants")
