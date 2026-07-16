#!/usr/bin/env python3
"""Validate Smack's hardware knob pages and on-device help."""

import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFESTS = (
    ROOT / "modules/audio_fx/smack/module.json",
    ROOT / "modules/sound_generators/smack-in/module.json",
)
EXPECTED_KNOBS = {
    "root": [
        "fx_density",
        "order_density",
        "loop_len",
        "slice_res",
        "wet",
        "reroll",
        "ab",
        "seed",
    ],
    "perform": [
        "capture",
        "arm",
        "ab",
        "reroll",
        "fx_density",
        "order_density",
        "wet",
        "seed",
    ],
    "setup": [
        "wet",
        "loop_len",
        "pad_rate",
        "seed",
        "slice_res",
        "channel_mode",
        "pan_l",
        "pan_r",
    ],
}

CHAIN_UI_PRIMARY_KNOBS = [
    "fx_density",
    "order_density",
    "loop_len",
    "slice_res",
    "wet",
    "reroll",
    "ab",
    "seed",
]
CHAIN_UI_SHIFT_KNOBS = [
    "pan_l",
    "pan_r",
    "channel_mode",
    "loop_len",
    "bpm_override",
    "monitor",
    "pad_play",
    "pad_rate",
]


def check_manifest(path: Path) -> None:
    data = json.loads(path.read_text())
    levels = data["capabilities"]["ui_hierarchy"]["levels"]
    definitions = {
        param["key"]: param
        for level in levels.values()
        for param in level.get("params", [])
        if isinstance(param, dict) and "key" in param
    }

    for level_name, expected in EXPECTED_KNOBS.items():
        actual = levels[level_name].get("knobs", [])
        assert actual == expected, f"{path}: {level_name} knobs: {actual!r}"
        assert len(actual) == 8, f"{path}: {level_name} must map all 8 knobs"
        for key in actual:
            assert key in definitions, f"{path}: unresolved knob param {key!r}"
            options = definitions[key].get("options")
            if options == ["idle", "trigger"]:
                allowed_triggers = {
                    "root": {"reroll"},
                    "perform": {"capture", "arm", "reroll"},
                }
                assert key in allowed_triggers.get(level_name, set()), (
                    f"{path}: unexpected trigger knob {level_name}.{key}"
                )

    assert levels["perform"]["knobs"][:4] == ["capture", "arm", "ab", "reroll"], (
        f"{path}: performance actions must occupy knobs 1-4"
    )
    assert all("quantize" not in level.get("knobs", []) for level in levels.values()), (
        f"{path}: A/B Quantize must remain list-only"
    )
    assert all("pitch_range" not in level.get("knobs", []) for level in levels.values()), (
        f"{path}: Pitch Range must remain list-only"
    )
    assert all("transport" not in level.get("knobs", []) for level in levels.values()), (
        f"{path}: Transport must remain list-only"
    )
    assert all("detect_bpm" not in level.get("knobs", []) for level in levels.values()), (
        f"{path}: Detect BPM must remain list-only"
    )

    for level_name, level in levels.items():
        for param in level.get("params", []):
            if isinstance(param, str):
                assert param in definitions, (
                    f"{path}: {level_name} references undefined param {param!r}"
                )

    seed = definitions["seed"]
    assert seed["min"] == 1 and seed["max"] == 9999
    assert seed.get("knob_acceleration") == "wide", (
        f"{path}: Seed must opt into wide-range knob acceleration"
    )


def iter_help_lines(value):
    if isinstance(value, dict):
        for key, child in value.items():
            if key == "lines":
                yield from child
            else:
                yield from iter_help_lines(child)
    elif isinstance(value, list):
        for child in value:
            yield from iter_help_lines(child)


def check_help(path: Path) -> None:
    data = json.loads(path.read_text())
    too_long = [line for line in iter_help_lines(data) if len(line) > 20]
    assert not too_long, f"{path}: help lines exceed 20 chars: {too_long!r}"


def extract_js_knob_keys(source: str, const_name: str) -> list[str]:
    marker = f"const {const_name} = ["
    start = source.index(marker) + len(marker)
    end = source.index("\n];", start)
    block = source[start:end]
    keys = []
    for line in block.splitlines():
        token = "key: '"
        if token in line:
            keys.append(line.split(token, 1)[1].split("'", 1)[0])
    return keys


def check_chain_ui(path: Path, *, allow_sparse_shift: bool = False) -> None:
    source = path.read_text()
    assert extract_js_knob_keys(source, "KNOBS") == CHAIN_UI_PRIMARY_KNOBS
    shift = extract_js_knob_keys(source, "KNOBS2")
    if allow_sparse_shift:
        assert shift == CHAIN_UI_SHIFT_KNOBS[:6]
    else:
        assert shift == CHAIN_UI_SHIFT_KNOBS
    assert source.count("trig: true") == 1, f"{path}: only Re-Roll may be a custom trigger knob"
    assert "function adjustDirectionalTrigger(i, k, delta)" in source
    assert "host_module_set_param(k.key, 'trigger')" in source
    assert "host_module_set_param(k.key, 'idle')" in source
    assert "accelerateSeedDelta(delta)" in source, (
        f"{path}: custom UI must accelerate Seed independently of host version"
    )


for manifest in MANIFESTS:
    check_manifest(manifest)

check_chain_ui(ROOT / "src/ui_chain.js")
check_chain_ui(ROOT / "src/ui_overtake.js", allow_sparse_shift=True)
check_help(ROOT / "src/help.json")
check_help(ROOT / "src/help_smack_in.json")
print("manifest UI validation passed")
