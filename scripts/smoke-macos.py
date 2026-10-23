#!/usr/bin/env python3
"""Run the bundled viewer against generated format and collection fixtures."""
import argparse
import json
import os
import pathlib
import plistlib
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("app", type=pathlib.Path)
parser.add_argument("--fixtures-tool", required=True, type=pathlib.Path)
parser.add_argument("--output", required=True, type=pathlib.Path)
args = parser.parse_args()
executable = args.app.resolve() / "Contents/MacOS/ikview"
with (args.app.resolve() / "Contents/Info.plist").open("rb") as metadata:
    bundle_version = plistlib.load(metadata)["CFBundleShortVersionString"]
args.output.mkdir(parents=True, exist_ok=True)
(args.output / "summary.json").unlink(missing_ok=True)

def rgba(value):
    raw = value.lstrip("#")
    return tuple(int(raw[i:i + 2], 16) for i in (0, 2, 4, 6))

results = []
with tempfile.TemporaryDirectory(prefix="ikview-formats-") as working:
    directory = pathlib.Path(working)
    fixtures = directory / "fixtures"
    subprocess.run([str(args.fixtures_tool.resolve()), str(fixtures)], check=True, timeout=30)
    manifest = json.loads((fixtures / "manifest.json").read_text())
    for ordinal, case in enumerate(manifest):
        selected = directory / f"{ordinal:02d}-공백 폴더"
        selected.mkdir()
        source = selected / case["file"]
        shutil.copy2(fixtures / case["file"], source)
        attempts = [case]
        if case["file"] == "pages.cbz":
            # Reuse the same source and settings: explicit page 1 must override
            # the persisted second page from the first process.
            attempts.append(dict(case, page_number=1, center="#ffff0000"))
        for attempt, case in enumerate(attempts):
            suffix = "-reopened" if attempt else ""
            if attempt:
                assert "archivePositions" in (selected / "settings.ini").read_text(), "First process did not persist the archive position"
            prefix = args.output / f"{ordinal:02d}-{case['file']}{suffix}"
            report = pathlib.Path(str(prefix) + ".json")
            screenshot = pathlib.Path(str(prefix) + ".png")
            report.unlink(missing_ok=True)
            screenshot.unlink(missing_ok=True)
            environment = os.environ.copy()
            environment["IKVIEW_SETTINGS_PATH"] = str(selected / "settings.ini")
            environment["DYLD_PRINT_LIBRARIES"] = "1"
            # Only the bundle's plugins may satisfy the release smoke check.
            for variable in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH"):
                environment.pop(variable, None)
            for variable in ("DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "DYLD_FALLBACK_FRAMEWORK_PATH"):
                environment.pop(variable, None)
            command = [str(executable), str(source), "--inspect", str(report), "--screenshot", str(screenshot),
                       "--capture-after", "800", "--quit-after", "1200"]
            if case["image_number"] > 1:
                command += ["--image-number", str(case["image_number"])]
            if case["page_number"] > 1 or attempt:
                command += ["--page-number", str(case["page_number"])]
            execution = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=20)
            pathlib.Path(str(prefix) + ".log").write_text(execution.stdout + execution.stderr)
            assert execution.returncode == 0, f"{case['file']}: exit {execution.returncode}"
            state = json.loads(report.read_text())
            assert state["version"] == bundle_version, (state["version"], bundle_version)
            document = state["document"]
            assert not state["errors"], f"{case['file']}: {state['errors']}"
            assert document["image_ready"] and not document["busy"], f"{case['file']}: {document}"
            assert pathlib.Path(document["source_path"]).resolve() == source.resolve(), f"{case['file']}: wrong source {document['source_path']}"
            assert state["frame_ready"], f"{case['file']}: renderer did not present a frame"
            assert "Metal" in state["renderer"], state["renderer"]
            for key in ("count", "width", "height", "page_count"):
                assert document[key] == case[key], f"{case['file']}: {key}={document[key]}, expected {case[key]}"
            assert document["index"] == case["image_number"] - 1, document
            assert document["page"] == case["page_number"] - 1, document
            assert document["animated"] == case["animated"], document
            assert document["pdf"] == case.get("pdf", False), document
            if case["animated"]:
                assert document["observed_frame_count"] >= 2, document
                assert set(document["observed_center_colors"]) == {"#ffff0000", "#ff0000ff"}, document
            assert state["selection"] == [document["index"]], f"{case['file']}: sidebar {state['selection']}"
            if case["center"]:
                expected = rgba(case["center"])
                actual = rgba(document["center_pixel"])
                assert max(abs(a - b) for a, b in zip(actual, expected)) <= case["tolerance"], (case["file"], actual, expected)
                if expected[0] == 255:
                    displayed = rgba(state["frame_center_pixel"])
                    assert max(abs(a - b) for a, b in zip(displayed, expected)) <= max(case["tolerance"], 2), (case["file"], "display", displayed, expected)
            if "corner_colors" in case:
                assert len(document["corner_pixels"]) == len(case["corner_colors"]), document
                for actual, expected in zip(document["corner_pixels"], case["corner_colors"]):
                    assert max(abs(a-b) for a,b in zip(rgba(actual),rgba(expected))) <= case["tolerance"], (case["file"], "orientation", actual, expected)
            external = [line for line in execution.stderr.splitlines() if "dyld[" in line and any(path in line for path in ("/opt/homebrew/", "/usr/local/", "/ikview/build"))]
            assert not external, f"{case['file']}: development dependency loaded: {external}"
            assert screenshot.stat().st_size > 1024
            results.append({"file": case["file"], "passed": True, "scenario": "reopen-first-page" if attempt else "open", "version": state["version"], "document": document})
            print(f"PASS {case['file']}{suffix}", flush=True)
summary = {"passed": len(results), "failed": 0, "cases": results}
(args.output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
print(f"Verified {len(results)} bundled format/collection cases", flush=True)
