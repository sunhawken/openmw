"""Check live Motion Strength against the real controller and full Siff skin data."""
import argparse
import ctypes as C
import json
import math
from pathlib import Path
import xml.etree.ElementTree as ET

import numpy as np

from run_tests import Asset, Rig, ROOT, animate, matrix, run
from verify_final import SETTINGS, mesh_metrics

LIBRARIES = {}


def set_strength(library, value):
    if library not in LIBRARIES:
        lib = C.CDLL(str((ROOT / library).resolve()))
        lib.rig_set_motion_strength.argtypes = [C.c_float]
        LIBRARIES[library] = lib
    LIBRARIES[library].rig_set_motion_strength(float(value))


def check_layout():
    repo = ROOT.parents[1]
    layout = ET.parse(repo / "files/data/mygui/openmw_settings_window.layout")
    view = layout.find(".//Widget[@name='VerletScrollView']")
    slider = view.find("./Widget[@name='VerletMotionStrengthSlider']")
    props = {e.get("key"): e.get("value") for e in slider.findall("Property")}
    users = {e.get("key"): e.get("value") for e in slider.findall("UserString")}
    assert users["SettingName"] == "verlet movement influence"
    assert users["SettingMin"] == "0" and users["SettingMax"] == "8"
    assert int(props["Range"]) == 801
    assert 8 * 100 / (int(props["Range"]) - 1) == 1  # 1x is exactly selectable.
    labels = layout.findall(".//Widget[@name='VerletMotionStrengthText']")
    assert len(labels) == 1 and users["SettingLabelWidget"] == labels[0].get("name")
    assert len(view.findall(".//UserString[@key='SettingName'][@value='verlet movement influence']")) == 1
    # Check direct children against the scroll canvas and neighboring controls.
    rectangles = []
    canvas = list(map(int, view.find("./Property[@key='CanvasSize']").get("value").split()))
    for w in view.findall("./Widget"):
        if "position" not in w.attrib:
            continue
        x, y, width, height = map(int, w.get("position").split())
        assert x >= 0 and y >= 0 and x + width <= canvas[0] and y + height <= canvas[1]
        if y >= 72:
            rectangles.append((y, y + height))
    rectangles.sort()
    assert all(a[1] <= b[0] for a, b in zip(rectangles, rectangles[1:]))
    assert "verlet movement influence = 1.0" in (repo / "files/settings-default.cfg").read_text()


def root_tilt(asset, result, degrees):
    index = len(result["history"]) // 2
    p = result["history"][index]
    homogeneous = np.concatenate([p, np.ones((*p.shape[:2], 1))], axis=2)
    local = (homogeneous @ np.linalg.inv(result["worlds"][index, 0]))[:, :, :3]
    d = local[:, 3] - local[:, 2]
    u = np.array([math.sin(math.radians(degrees)), math.cos(math.radians(degrees)), 0])
    return float(np.median(np.degrees(np.arctan2(-(d @ u), -d[:, 2]))))


def live_changes(asset, library):
    set_strength(library, 0)
    rig = Rig(asset, library, SETTINGS[asset.name])
    poses = []
    lengths = np.linalg.norm(np.diff(asset.rest, axis=1), axis=2)[:, 2:]
    maximum = 1.0
    for frame in range(601):
        t = frame / 60
        strength = 0 if t < 1 else 8 if t < 3 else 0 if t < 4 else 8 if t < 5 else .25 if t < 6 else 4 if t < 7 else 1
        set_strength(library, strength)
        p = (28 * math.sin(min(t, 7) * 4), 320 * min(t, 7), 0)
        rig.set(0, matrix(p, angles=(0, 0, .7 * max(0, min(t, 7) - 3))))
        animate(rig, t * 11, float(t < 7))
        capture = frame in [90, 210, 270, 390, 510, 600]
        rig.tick(t, world=capture)
        assert np.isfinite(rig.positions).all()
        maximum = max(maximum, float((np.linalg.norm(np.diff(rig.positions, axis=1), axis=2)[:, 2:] / lengths).max()))
        if capture:
            poses.append(rig.world.copy())
    rig.close()
    return maximum, poses


def main(args):
    check_layout()
    report = {"scope": "Headless actual C++ controller with full Siff skin data; no gameplay/UI rendering.",
              "strengths": [0, .25, 1, 2, 4, 8], "assets": {}, "failures": []}
    for name in SETTINGS:
        asset = Asset(name)
        data = {"cases": [], "mesh": {}, "full_speed_tilt_degrees": {}}
        for strength in report["strengths"]:
            worlds = []
            tilts = []
            for degrees in range(0, 360, 45):
                set_strength(args.library, strength)
                result = run(asset, args.library, "full_run", math.radians(degrees), settings=SETTINGS[name], record=True)
                tilts.append(root_tilt(asset, result, degrees))
                worlds.extend(result["worlds"][[54, 90, 174]])
                result.pop("history"); result.pop("worlds")
                data["cases"].append({"strength": strength, **result})
            if strength in [0, 8]:
                for kind in ["walk", "jump"]:
                    for degrees in range(0, 360, 45):
                        set_strength(args.library, strength)
                        result = run(asset, args.library, kind, math.radians(degrees), settings=SETTINGS[name], record=True)
                        worlds.extend(result["worlds"][[54, 90, 135]])
                        result.pop("history"); result.pop("worlds")
                        data["cases"].append({"strength": strength, **result})
                for fps in [30, 120]:
                    set_strength(args.library, strength)
                    result = run(asset, args.library, "extreme", fps=fps, settings=SETTINGS[name], record=True)
                    worlds.extend(result["worlds"][::max(1, len(result["worlds"]) // 8)])
                    result.pop("history"); result.pop("worlds")
                    data["cases"].append({"strength": strength, **result})
            data["mesh"][str(strength)] = mesh_metrics(asset, np.array(worlds))
            data["full_speed_tilt_degrees"][str(strength)] = tilts
            if any(c["max_segment_ratio"] > 1.05 for c in data["cases"] if c["strength"] == strength):
                report["failures"].append(f"{name} length at {strength}x")
            if data["mesh"][str(strength)]["min_surface_band_width_ratio"] < .65:
                report["failures"].append(f"{name} mesh width at {strength}x")
            print(name, strength, "tilt", round(min(tilts), 2), round(max(tilts), 2),
                  "mesh width", round(data["mesh"][str(strength)]["min_surface_band_width_ratio"], 4), flush=True)
        tilts = [np.median(data["full_speed_tilt_degrees"][str(s)]) for s in report["strengths"]]
        if any(a >= b for a, b in zip(tilts, tilts[1:])):
            report["failures"].append(name + " non-increasing lift")
        if tilts[-1] < tilts[2] + 10 or tilts[2] < tilts[0] + 30:
            report["failures"].append(name + " insufficient adjustment range")
        maximum, poses = live_changes(asset, args.library)
        data["live"] = {"max_segment_ratio": maximum, "mesh": mesh_metrics(asset, np.array(poses))}
        if maximum > 1.05 or data["live"]["mesh"]["min_surface_band_width_ratio"] < .65:
            report["failures"].append(name + " live slider changes")
        # Raw setting values outside the UI range are also bounded by the controller.
        for value, endpoint in [(-2, 0), (99, 8)]:
            set_strength(args.library, value)
            a = run(asset, args.library, "full_run", settings=SETTINGS[name], record=True)
            set_strength(args.library, endpoint)
            b = run(asset, args.library, "full_run", settings=SETTINGS[name], record=True)
            if not np.array_equal(a["history"], b["history"]):
                report["failures"].append(name + " setting bounds")
        # Remove the forces without removing root-velocity tracking, which also
        # controls floor-contact draping. An all-zero metadata profile disables
        # that tracking and is therefore a different collision scenario.
        set_strength(args.library, 0)
        zero = run(asset, args.library, "full_run", settings=SETTINGS[name], record=True)
        settings = SETTINGS[name].copy()
        settings[7] = settings[9] = settings[11] = 0
        set_strength(args.library, 1)
        disabled = run(asset, args.library, "full_run", settings=settings, record=True)
        if not np.array_equal(zero["history"], disabled["history"]):
            report["failures"].append(name + " zero strength")
        # Profiles with no authored motion forces stay unaffected by the slider.
        settings = SETTINGS[name].copy()
        settings[6] = settings[9] = settings[11] = 0
        set_strength(args.library, 1)
        disabled = run(asset, args.library, "full_run", settings=settings, record=True)
        set_strength(args.library, 8)
        unaffected = run(asset, args.library, "full_run", settings=settings, record=True)
        if not np.array_equal(disabled["history"], unaffected["history"]):
            report["failures"].append(name + " unconfigured profile")
        if args.baseline_library:
            errors = []
            for kind in ["full_run", "jump", "extreme"]:
                for degrees in [0, 180]:
                    set_strength(args.library, 1)
                    a = run(asset, args.library, kind, math.radians(degrees), settings=SETTINGS[name], record=True)
                    set_strength(args.baseline_library, 1)
                    b = run(asset, args.baseline_library, kind, math.radians(degrees), settings=SETTINGS[name], record=True)
                    errors.append(float(np.abs(a["history"] - b["history"]).max()))
                    errors.append(float(np.abs(a["worlds"] - b["worlds"]).max()))
            data["default_baseline_max_error"] = max(errors)
            if max(errors) != 0:
                report["failures"].append(name + " changed 1x tuning")
        report["assets"][name] = data
    set_strength(args.library, 1)
    report["passed"] = not report["failures"]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2))
    print("PASS" if report["passed"] else "FAIL", report["failures"], flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", default="final.so")
    parser.add_argument("--baseline-library")
    parser.add_argument("--output", type=Path, default=ROOT / "Motion-Strength-Report.json")
    raise SystemExit(main(parser.parse_args()))
