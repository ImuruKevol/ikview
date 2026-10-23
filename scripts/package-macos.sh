#!/bin/bash
set -euo pipefail
task_skip_gui_smoke=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --skip-gui-smoke) task_skip_gui_smoke=1; shift;;
        --help|-h)
            printf '%s\n' 'Usage: scripts/package-macos.sh [--skip-gui-smoke]' \
                '--skip-gui-smoke skips application-launching smoke tests and records that omission in the package validation manifest.'
            exit 0;;
        *) printf 'IKView package: unknown option: %s\n' "$1" >&2; exit 64;;
    esac
done
task_root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$task_root/dist"
task_staging="$(mktemp -d "$task_root/dist/.ikview-package.XXXXXX")"
task_app="$task_staging/ikview.app"
trap 'if test ! -f "$task_staging/KEEP_FOR_RECOVERY"; then rm -rf "$task_staging"; else echo "Recoverable release artifacts retained at $task_staging" >&2; fi' EXIT
ditto "$task_root/build/ikview.app" "$task_app"
task_brew_prefix="$(brew --prefix)"
macdeployqt "$task_app" -always-overwrite -no-codesign -libpath="$task_brew_prefix/lib" -libpath="$task_brew_prefix/opt/qtbase/lib" -libpath="$task_brew_prefix/opt/qtdeclarative/lib" -libpath="$task_brew_prefix/opt/qtsvg/lib"
# macdeployqt finds the SVG icon engine but can omit the image reader.
task_plugin_root="$(qtpaths --query QT_INSTALL_PLUGINS)"
test -f "$task_plugin_root/imageformats/libqsvg.dylib"
mkdir -p "$task_app/Contents/PlugIns/imageformats"
cp -L "$task_plugin_root/imageformats/libqsvg.dylib" "$task_app/Contents/PlugIns/imageformats/libqsvg.dylib"
python3 "$task_root/scripts/bundle-dylibs.py" "$task_app"
python3 "$task_root/scripts/collect-licenses.py" "$task_app"
task_sign_identity="${IKVIEW_CODESIGN_IDENTITY:--}"
# Sign dependencies before their containing bundles. Reapply the Finder
# sandbox explicitly: a recursive signature must not silently drop it.
codesign --force --deep --sign "$task_sign_identity" "$task_app"
task_finder_extension="$task_app/Contents/PlugIns/IKViewFinder.appex"
test -x "$task_finder_extension/Contents/MacOS/IKViewFinder"
codesign --force --sign "$task_sign_identity" --entitlements "$task_root/assets/finder/entitlements.plist" "$task_finder_extension"
codesign --force --sign "$task_sign_identity" "$task_app"
codesign --verify --deep --strict "$task_app"
if [ "$task_skip_gui_smoke" -eq 1 ]; then
    printf '%s\n' 'GUI smoke tests SKIPPED by explicit --skip-gui-smoke. No viewer is launched by package validation.' \
        'Any existing test-output/package-smoke/summary.json belongs to an earlier run and is not a result for this package.'
else
    python3 "$task_root/scripts/smoke-macos.py" "$task_app" --fixtures-tool "$task_root/build/ikview_fixtures" --output "$task_root/test-output/package-smoke"
fi
ditto -c -k --sequesterRsrc --keepParent "$task_app" "$task_staging/ikview-macos-arm64.zip"
python3 - "$task_staging" "$task_skip_gui_smoke" "$task_root/test-output/package-smoke/summary.json" <<'PY'
import datetime, hashlib, json, pathlib, plistlib, sys
stage, skip, summary_path = pathlib.Path(sys.argv[1]), sys.argv[2] == "1", pathlib.Path(sys.argv[3])
def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()
info = plistlib.loads((stage / "ikview.app/Contents/Info.plist").read_bytes())
archive = stage / "ikview-macos-arm64.zip"
smoke = {"status": "skipped" if skip else "passed", "executed_for_this_package": not skip}
if skip:
    smoke.update(reason="Explicit --skip-gui-smoke", passed=None, failed=None,
                 summary_path=None, historical_summary_reused=False)
else:
    summary = json.loads(summary_path.read_text())
    if summary.get("failed") != 0 or summary.get("passed", 0) < 1:
        raise RuntimeError("GUI smoke summary is not a successful result for this invocation")
    smoke.update(passed=summary["passed"], failed=summary["failed"], summary_path=str(summary_path),
                 summary_sha256=sha256(summary_path))
manifest = {
    "schema_version": 1,
    "created_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "version": info["CFBundleShortVersionString"],
    "main_executable_sha256": sha256(stage / "ikview.app/Contents/MacOS" / info["CFBundleExecutable"]),
    "archive": {"name": archive.name, "bytes": archive.stat().st_size, "sha256": sha256(archive)},
    "validation": {"strict_deep_signature": "passed", "bundled_imports": "passed", "gui_smoke": smoke},
}
(stage / "ikview-package-validation.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
PY
# Publish only after every validation succeeded. Restore both prior artifacts
# if any filesystem rename fails during publication.
python3 - "$task_staging" "$task_root/dist" <<'PY'
import os, pathlib, shutil, sys
stage, destination = map(pathlib.Path, sys.argv[1:])
names = ("ikview.app", "ikview-macos-arm64.zip", "ikview-package-validation.json")
moved, installed = [], []
marker = stage / "KEEP_FOR_RECOVERY"
marker.touch()
try:
    for name in names:
        existing = destination / name
        if existing.exists():
            os.replace(existing, stage / ("previous-" + name))
            moved.append(name)
        os.replace(stage / name, existing)
        installed.append(name)
except BaseException:
    for name in reversed(installed):
        existing = destination / name
        shutil.rmtree(existing) if existing.is_dir() else existing.unlink()
    for name in reversed(moved):
        os.replace(stage / ("previous-" + name), destination / name)
    marker.unlink()
    raise
marker.unlink()
PY
echo "$task_root/dist/ikview.app"
echo "$task_root/dist/ikview-package-validation.json"
