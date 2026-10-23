#!/usr/bin/env python3
"""Package committed IKView code with the exact bundled dependency sources."""
import argparse
import datetime
import hashlib
import io
import json
import os
import pathlib
import plistlib
import subprocess
import tarfile
import tempfile


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verified_inputs(manifest, cache):
    """Reject incomplete or modified cached material before replacing a release."""
    result = []
    names = set()
    for dependency in manifest["dependencies"]:
        name = dependency["name"]
        if pathlib.PurePosixPath(name).name != name or name in names:
            raise RuntimeError(f"Invalid or duplicate dependency name: {name}")
        names.add(name)
        if len([s for s in dependency["sources"] if s["kind"] == "source"]) != 1:
            raise RuntimeError(f"Missing unique upstream source: {name}")
        for source in dependency["sources"]:
            relative = pathlib.PurePosixPath(source["file"])
            if relative.is_absolute() or ".." in relative.parts or len(relative.parts) != 2 or relative.parts[0] != name:
                raise RuntimeError(f"Invalid source filename: {source['file']}")
            if len(source["sha256"]) != 64 or any(c not in "0123456789abcdef" for c in source["sha256"]):
                raise RuntimeError("Invalid source SHA-256")
            result.append((cache / "downloads" / source["sha256"], "dependencies/" + source["file"], source["sha256"]))
        for filename, checksum in [("formula.rb", dependency["recipe_sha256"]), ("sbom.spdx.json", dependency["sbom_sha256"])]:
            result.append((cache / "recipes" / name / filename, f"dependencies/{name}/{filename}", checksum))
    if not names:
        raise RuntimeError("No dependency sources in manifest")
    if len({r[1] for r in result}) != len(result):
        raise RuntimeError("Duplicate archive filename")
    for path, _, checksum in result:
        if not path.is_file() or sha256(path) != checksum:
            raise RuntimeError(f"Missing or changed source input: {path.name}; run package-macos.sh again")
    return result


def normalize(info):
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    return info


def add_text(archive, name, text):
    data = text.encode("utf-8")
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = 0o644
    archive.addfile(info, io.BytesIO(data))


def package(root, app, destination):
    if subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True).strip():
        raise RuntimeError("Commit the release changes before packaging corresponding source")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    notices = app / "Contents/Resources/third-party"
    manifest = json.loads((notices / "source-manifest.json").read_text())
    if manifest.get("project_dirty") is not False or manifest.get("project_commit") != commit:
        raise RuntimeError("Package the app again from this clean commit before packaging corresponding source")
    with (app / "Contents/Info.plist").open("rb") as source:
        info = plistlib.load(source)
    version = info["CFBundleShortVersionString"]
    if not all(c in "0123456789.-abcdefghijklmnopqrstuvwxyz" for c in version) or not version:
        raise RuntimeError("Invalid application version")
    license_bytes = subprocess.check_output(["git", "show", "HEAD:LICENSE"], cwd=root)
    if (app / "Contents/Resources/LICENSE").read_bytes() != license_bytes:
        raise RuntimeError("Packaged project license differs from committed LICENSE")
    inputs = verified_inputs(manifest, root / "build/license-sources")
    prefix = f"ikview-{version}"
    archive_name = prefix + "-source.tar.gz"
    project = subprocess.check_output(["git", "archive", "--format=tar", "HEAD"], cwd=root)
    executable = app / "Contents/MacOS" / info["CFBundleExecutable"]
    provenance = {"schema_version": 1, "version": version, "project_commit": commit,
                  "project_license": "GPL-3.0-or-later", "main_executable_sha256": sha256(executable),
                  "dependency_count": len(manifest["dependencies"]),
                  "source_manifest_sha256": sha256(notices / "source-manifest.json")}
    readme = f"""# IKView {version} 해당 소스

IKView 커밋: `{commit}`. 프로젝트 라이선스는 GPL-3.0-or-later입니다.

- `ikview/`: 해당 커밋의 코드·문서·프로젝트 제작 이미지·LICENSE·빌드 스크립트.
- `dependencies/`: 번들 라이브러리의 원본 소스 압축파일, 적용 패치, Homebrew 빌드 레시피와 설치 버전의 SBOM.
- `source-manifest.json`: 원본 다운로드 URL·버전·SHA-256·Homebrew 버전.
- `SHA256SUMS`: 동봉한 의존성 소스와 빌드 레시피의 해시.

IKView 빌드 절차는 `ikview/docs/building.md`에 있습니다. 라이브러리를 직접 빌드할 때는 각 `upstream.*` 압축파일을 풀고 `formula.rb`에 기록된 순서로 동봉한 패치를 적용합니다. 레시피에는 빌드 옵션과 소스 수정 사항이 포함되어 있습니다. Homebrew의 표준 빌드 도구·CMake·컴파일러·macOS SDK가 별도로 필요합니다. 원본 소스의 라이선스·저작권 고지를 유지해야 합니다.

설치 파일 다운로드 위치에서 이 소스 묶음도 함께 제공하세요. 외부 의존성에는 각 권리자의 라이선스가 적용됩니다. Homebrew 빌드 레시피의 라이선스는 `Homebrew-LICENSE.txt`에 보존했습니다. 앱에서 열거나 변환한 사용자 파일은 이 소스 묶음에 포함되지 않습니다.
"""
    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".ikview-sources-", dir=destination) as temporary:
        staging = pathlib.Path(temporary)
        archive_path = staging / archive_name
        with tarfile.open(archive_path, "w:gz", compresslevel=6) as archive:
            with tarfile.open(fileobj=io.BytesIO(project), mode="r:") as tracked:
                for member in tracked.getmembers():
                    member.name = prefix + "/ikview/" + member.name
                    archive.addfile(normalize(member), tracked.extractfile(member) if member.isfile() else None)
            for path, name, _ in inputs:
                archive.add(path, arcname=prefix + "/" + name, recursive=False, filter=normalize)
            archive.add(notices / "Homebrew-LICENSE.txt", arcname=prefix + "/Homebrew-LICENSE.txt", filter=normalize)
            archive.add(notices / "source-manifest.json", arcname=prefix + "/source-manifest.json", filter=normalize)
            add_text(archive, prefix + "/README.md", readme)
            add_text(archive, prefix + "/provenance.json", json.dumps(provenance, ensure_ascii=False, indent=2) + "\n")
            add_text(archive, prefix + "/SHA256SUMS", "".join(f"{s}  {n}\n" for _, n, s in inputs))
        validation = {**provenance, "created_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      "archive": {"name": archive_name, "bytes": archive_path.stat().st_size, "sha256": sha256(archive_path)},
                      "validation": {"clean_committed_project": "passed", "dependency_source_checksums": "passed",
                                     "packaged_project_license": "passed"}}
        record = staging / "ikview-source-validation.json"
        record.write_text(json.dumps(validation, ensure_ascii=False, indent=2) + "\n")
        os.replace(archive_path, destination / archive_name)
        os.replace(record, destination / record.name)
    print(destination / archive_name)
    print(destination / "ikview-source-validation.json")


if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=pathlib.Path, default=root / "dist/ikview.app")
    parser.add_argument("--output", type=pathlib.Path, default=root / "dist")
    args = parser.parse_args()
    package(root, args.app.resolve(), args.output.resolve())
