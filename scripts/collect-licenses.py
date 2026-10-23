#!/usr/bin/env python3
"""Collect verified dependency sources, patches and original license notices."""
import concurrent.futures
import hashlib
import json
import os
import pathlib
import plistlib
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request


def bundled_qt_version(app):
    info = app / "Contents/Frameworks/QtCore.framework/Resources/Info.plist"
    with info.open("rb") as source:
        version = plistlib.load(source)["CFBundleVersion"]
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-(?:alpha|beta|rc)\d+)?", version):
        raise RuntimeError(f"Cannot identify an exact Qt source tag from bundled version: {version}")
    return version


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def contains_bundled_library(formula_root, bundled):
    # Image-format modules can ship only plugins, outside lib/*.framework.
    libraries = list(formula_root.rglob("*.dylib")) + list((formula_root / "lib").glob("*.framework"))
    return any(p.name in bundled for p in libraries)


def source_records(formula_root, name):
    """Read the installed bottle's SBOM, never the latest Homebrew formula."""
    sbom = json.loads((formula_root / "sbom.spdx.json").read_text())
    receipt = json.loads((formula_root / "INSTALL_RECEIPT.json").read_text())
    version = receipt["source"]["versions"]["stable"]
    candidates = [p for p in sbom["packages"] if p.get("name") == name
                  and p.get("versionInfo") == version
                  and p.get("SPDXID", "").startswith("SPDXRef-Archive-")]
    if len(candidates) != 1:
        raise RuntimeError(f"Cannot identify exact installed source: {name} {version}")
    upstream = candidates[0]
    patches = [p for p in sbom["packages"] if any(
        r.get("spdxElementId") == p.get("SPDXID") and r.get("relationshipType") == "PATCH_APPLIED"
        and r.get("relatedSpdxElement") == upstream["SPDXID"] for r in sbom.get("relationships", []))]
    # Source resources (if any) are also retained; bottle dependencies are not
    # source archives and are collected separately from their own installed keg.
    resources = [p for p in sbom["packages"] if p.get("SPDXID", "").startswith("SPDXRef-Resource-")]
    recipe = (formula_root / ".brew" / (name + ".rb")).read_text()
    mirrors = re.findall(r'^\s*mirror "(https://[^"\n]+)"', recipe, re.MULTILINE)
    records = []
    for index, package in enumerate([upstream] + patches + resources):
        url = package.get("downloadLocation", "")
        hashes = [c["checksumValue"] for c in package.get("checksums", []) if c["algorithm"] == "SHA256"]
        if not url.startswith("https://") or len(hashes) != 1 or not re.fullmatch(r"[0-9a-f]{64}", hashes[0]):
            raise RuntimeError(f"Missing HTTPS source and SHA-256: {name} {package.get('name')}")
        url_path = urllib.parse.urlsplit(url).path
        suffixes = next((s for s in (".tar.gz", ".tar.xz", ".tar.bz2", ".tgz", ".zip") if url_path.endswith(s)),
                        pathlib.PurePosixPath(url_path).suffix)
        kind = "source" if index == 0 else ("patch" if package in patches else "resource")
        filename = "upstream" + suffixes if index == 0 else f"{kind}-{index}" + (".patch" if kind == "patch" else suffixes)
        records.append({"kind": kind, "url": url, "sha256": hashes[0], "file": name + "/" + filename,
                        "mirrors": mirrors if index == 0 else []})
    return version, records


def fetch_verified(record, cache):
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / record["sha256"]
    if target.exists() and sha256(target) == record["sha256"]:
        return target
    errors = []
    for url in [record["url"]] + record.get("mirrors", []):
        temporary = None
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "IKView-source-packager/1.0"})
            with urllib.request.urlopen(request, timeout=30) as response:
                with tempfile.NamedTemporaryFile(dir=cache, delete=False) as output:
                    temporary = pathlib.Path(output.name)
                    shutil.copyfileobj(response, output)
            if sha256(temporary) != record["sha256"]:
                raise RuntimeError("SHA-256 mismatch")
            os.replace(temporary, target)
            return target
        except (OSError, RuntimeError) as error:
            errors.append(f"{url}: {error}")
        finally:
            if temporary and temporary.exists():
                temporary.unlink()
    raise RuntimeError("Cannot obtain verified source: " + "; ".join(errors))


def copy_source_notices(archive, target):
    """Read regular text members only; never extract links or archive paths."""
    with tarfile.open(archive, "r:*") as source:
        members = {}
        for item in source.getmembers():
            path = pathlib.PurePosixPath(item.name)
            if path.is_absolute() or ".." in path.parts:
                raise RuntimeError(f"Unsafe source archive path: {item.name}")
            if item.isfile() and len(path.parts) > 1:
                members[pathlib.PurePosixPath(*path.parts[1:])] = item
        selected = {p for p in members if any(w in p.name.upper() for w in
                    ("LICENSE", "COPYING", "NOTICE", "COPYRIGHT", "AUTHORS"))
                    or "LICENSES" in [part.upper() for part in p.parts]
                    or p.name.upper() in ("FTL.TXT", "GPLV2.TXT", "README.IJG", "QT_ATTRIBUTION.JSON")}
        for path in list(selected):
            if path.name != "qt_attribution.json":
                continue
            raw = source.extractfile(members[path]).read()
            # Qt attribution metadata permits literal line breaks in text.
            # Preserve the original bytes and only relax string parsing here.
            entries = json.loads(raw, strict=False)
            for entry in entries if isinstance(entries, list) else [entries]:
                filenames = entry.get("LicenseFile", "")
                for filename in filenames if isinstance(filenames, list) else filenames.split():
                    referenced = pathlib.PurePosixPath(posixpath.normpath(str(path.parent / filename)))
                    if referenced in members:
                        selected.add(referenced)
        copied = []
        for path in sorted(selected):
            item = members[path]
            if item.size > 4 * 1024 * 1024:
                raise RuntimeError(f"Unexpectedly large notice: {item.name}")
            data = source.extractfile(item).read()
            if not data:
                continue
            output = target / path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(data)
            copied.append(str(path))
    if not copied:
        raise RuntimeError(f"No license notices in source archive: {archive.name}")
    return copied


def collect(app):
    app = app.resolve()
    root = pathlib.Path(__file__).resolve().parent.parent
    destination = app / "Contents/Resources/third-party"
    source_root = root / "build/license-sources"
    cache = source_root / "downloads"
    data = json.loads(subprocess.check_output(["brew", "info", "--json=v2", "--installed"], text=True))
    cellar = pathlib.Path(subprocess.check_output(["brew", "--cellar"], text=True).strip())
    bundled = {p.name for directory in ("Frameworks", "PlugIns")
               for p in (app / "Contents" / directory).rglob("*")}
    dependencies = []
    for formula in data["formulae"]:
        installed = formula.get("installed", [])
        version = formula.get("linked_keg") or (installed[-1]["version"] if installed else None)
        if not version:
            continue
        formula_root = cellar / formula["name"] / version
        if not contains_bundled_library(formula_root, bundled):
            continue
        source_version, records = source_records(formula_root, formula["name"])
        # The installed SBOM also fixes the license expression, unlike brew's
        # current online metadata, which can already describe a newer release.
        sbom = json.loads((formula_root / "sbom.spdx.json").read_text())
        source_package = next(p for p in sbom["packages"] if p.get("name") == formula["name"]
                              and p.get("versionInfo") == source_version and p.get("SPDXID", "").startswith("SPDXRef-Archive-"))
        dependencies.append({"name": formula["name"], "version": version, "source_version": source_version,
                             "license": source_package.get("licenseConcluded"), "homepage": formula.get("homepage"),
                             "sources": records, "formula_root": formula_root})
    if not dependencies:
        raise RuntimeError("No bundled Homebrew libraries identified")
    qt_version = bundled_qt_version(app)
    if any(d["name"].startswith("qt") and d["source_version"] != qt_version for d in dependencies):
        raise RuntimeError("Bundled Qt version differs from installed source metadata")
    records = [r for d in dependencies for r in d["sources"]]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda r: fetch_verified(r, cache), records))
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    shutil.copy2(root / "THIRD_PARTY.md", destination / "README.md")
    shutil.copy2(root / "LICENSE", app / "Contents/Resources/LICENSE")
    shutil.copy2(root / "COPYRIGHT", app / "Contents/Resources/COPYRIGHT")
    (destination / "docs").mkdir()
    shutil.copy2(root / "docs/licensing.md", destination / "docs/licensing.md")
    # Keep the repository's relative documentation links usable in the bundle.
    shutil.copy2(root / "LICENSE", destination / "LICENSE")
    shutil.copy2(root / "THIRD_PARTY.md", destination / "THIRD_PARTY.md")
    shutil.copy2(root / "docs/building.md", destination / "docs/building.md")
    brew_root = pathlib.Path(subprocess.check_output(["brew", "--repository"], text=True).strip())
    shutil.copy2(brew_root / "LICENSE.txt", destination / "Homebrew-LICENSE.txt")
    for dependency in dependencies:
        name = dependency["name"]
        formula_root = dependency.pop("formula_root")
        recipe_target = source_root / "recipes" / name
        recipe_target.mkdir(parents=True, exist_ok=True)
        for original, filename in [(formula_root / ".brew" / (name + ".rb"), "formula.rb"),
                                   (formula_root / "sbom.spdx.json", "sbom.spdx.json")]:
            shutil.copy2(original, recipe_target / filename)
        dependency["recipe_sha256"] = sha256(recipe_target / "formula.rb")
        dependency["sbom_sha256"] = sha256(recipe_target / "sbom.spdx.json")
        notices = copy_source_notices(cache / dependency["sources"][0]["sha256"], destination / name)
        dependency["notice_count"] = len(notices)
        # License entry points in the full source must be present, not just a
        # root README that points to another, missing license document.
        required = {"freetype": "FTL.TXT", "jpeg-turbo": "README.ijg", "gettext": "COPYING.LIB"}.get(name)
        if required and not any(pathlib.PurePosixPath(n).name == required for n in notices):
            raise RuntimeError(f"Missing required notice: {name}/{required}")
        print(f"{name} {dependency['version']}: {len(notices)} notice files", flush=True)
    qt_target = destination / "qt"
    qt_target.mkdir()
    for name in ("LGPL-3.0-only", "GPL-2.0-only", "GPL-3.0-only", "Qt-GPL-exception-1.0"):
        shutil.copy2(destination / "qtbase/LICENSES" / (name + ".txt"), qt_target / (name + ".txt"))
    (qt_target / "source.json").write_text(json.dumps({"version": qt_version, "tag": "v" + qt_version,
        "url": "https://github.com/qt/qtbase/tree/v" + qt_version + "/LICENSES"}, indent=2) + "\n")
    (destination / "libraries.json").write_text(json.dumps(dependencies, ensure_ascii=False, indent=2) + "\n")
    manifest = {"schema_version": 1, "dependencies": dependencies,
                "homebrew_version": subprocess.check_output(["brew", "--version"], text=True).splitlines()[0],
                "project_license": "GPL-3.0-or-later",
                "project_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                "project_dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True))}
    (destination / "source-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(f"Collected original sources and notices for {len(dependencies)} dependencies; Qt {qt_version}")


if __name__ == "__main__":
    collect(pathlib.Path(sys.argv[1]))
