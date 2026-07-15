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
        "loop_len",
        "slice_res",
        "fx_density",
        "order_density",
        "wet",
        "pitch_range",
        "quantize",
        "seed",
    ],
    "perform": [
        "ab",
        "fx_density",
        "order_density",
        "wet",
        "pitch_range",
        "quantize",
        "pad_play",
        "pad_rate",
    ],
    "setup": [
        "wet",
        "pitch_range",
        "quantize",
        "seed",
        "transport",
        "channel_mode",
        "pan_l",
        "pan_r",
    ],
}


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
            assert options != ["idle", "trigger"], (
                f"{path}: trigger param {key!r} must not be knob-mapped; "
                "Schwung 0.11.4 latches trigger gestures"
            )

    for level_name, level in levels.items():
        for param in level.get("params", []):
            if isinstance(param, str):
                assert param in definitions, (
                    f"{path}: {level_name} references undefined param {param!r}"
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


for manifest in MANIFESTS:
    check_manifest(manifest)

check_help(ROOT / "src/help.json")
check_help(ROOT / "src/help_smack_in.json")
print("manifest UI validation passed")
