#!/usr/bin/env python3
"""Offline regression checks for release license and corresponding-source tools."""
import hashlib
import importlib.util
import io
import json
import pathlib
import plistlib
import subprocess
import tarfile
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parent.parent


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


collector = module("collect_licenses", "collect-licenses.py")
packager = module("package_sources", "package-sources.py")


class LicenseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)

    def archive(self, files):
        path = self.root / "source.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for name, data in files.items():
                member = tarfile.TarInfo(name)
                member.size = len(data)
                archive.addfile(member, io.BytesIO(data))
        return path

    def test_nested_license_and_ijg_entry_points(self):
        archive = self.archive({"release/LICENSE.TXT": b"see docs/FTL.TXT",
                                "release/docs/FTL.TXT": b"FreeType full license",
                                "release/README.ijg": b"IJG legal terms", "release/code.cpp": b"code"})
        target = self.root / "notices"
        copied = collector.copy_source_notices(archive, target)
        self.assertEqual(set(copied), {"LICENSE.TXT", "docs/FTL.TXT", "README.ijg"})
        self.assertEqual((target / "docs/FTL.TXT").read_bytes(), b"FreeType full license")

    def test_plugin_only_qt_module_is_collected(self):
        directory = self.root / "qtimageformats/share/qt/plugins/imageformats"
        directory.mkdir(parents=True)
        (directory / "libqtiff.dylib").write_bytes(b"plugin")
        self.assertTrue(collector.contains_bundled_library(self.root / "qtimageformats", {"libqtiff.dylib"}))
        self.assertFalse(collector.contains_bundled_library(self.root / "qtimageformats", {"libqsvg.dylib"}))

    def test_qt_attribution_custom_license_filename(self):
        archive = self.archive({"qt/src/example/qt_attribution.json": b'{"LicenseFile": "LEGAL", "Description": "line 1\nline 2"}',
                                "qt/src/example/LEGAL": b"copyright and custom terms"})
        target = self.root / "notices"
        collector.copy_source_notices(archive, target)
        self.assertEqual((target / "src/example/LEGAL").read_bytes(), b"copyright and custom terms")

    def test_archive_traversal_rejected(self):
        for name in ["../LICENSE", "/LICENSE", "root/../../LICENSE"]:
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                collector.copy_source_notices(self.archive({name: b"bad"}), self.root / "notices")
        self.assertFalse((self.root / "LICENSE").exists())

    def test_empty_archive_notice_rejected(self):
        with self.assertRaises(RuntimeError):
            collector.copy_source_notices(self.archive({"root/LICENSE": b""}), self.root / "notices")

    def test_source_download_checksum_failure_does_not_replace_cache(self):
        data = b"verified source"
        record = {"url": "https://example.invalid/source.tar.gz", "sha256": hashlib.sha256(data).hexdigest()}
        cache = self.root / "cache"
        cache.mkdir()
        existing = cache / record["sha256"]
        existing.write_bytes(b"old incomplete cache")
        with mock.patch.object(collector.urllib.request, "urlopen", return_value=io.BytesIO(b"wrong response")):
            with self.assertRaisesRegex(RuntimeError, "SHA-256 mismatch"):
                collector.fetch_verified(record, cache)
        self.assertEqual(existing.read_bytes(), b"old incomplete cache")
        self.assertEqual(list(cache.iterdir()), [existing])

    def test_verified_cache_never_uses_network(self):
        data = b"verified source"
        record = {"url": "https://example.invalid/source.tar.gz", "sha256": hashlib.sha256(data).hexdigest()}
        (self.root / record["sha256"]).write_bytes(data)
        with mock.patch.object(collector.urllib.request, "urlopen", side_effect=AssertionError("Unexpected network")):
            self.assertEqual(collector.fetch_verified(record, self.root).read_bytes(), data)

    def test_installed_source_version_and_patch_not_latest_formula(self):
        source = {"SPDXID": "SPDXRef-Archive-qtbase-src", "name": "qtbase", "versionInfo": "6.7.1",
                  "downloadLocation": "https://example.invalid/qtbase-6.7.1.tar.xz",
                  "checksums": [{"algorithm": "SHA256", "checksumValue": "a" * 64}]}
        patch = {"SPDXID": "SPDXRef-Patch-qtbase-0", "name": "qtbase patch 0",
                 "downloadLocation": "https://example.invalid/fix.patch", "checksums": [{"algorithm": "SHA256", "checksumValue": "b" * 64}]}
        sbom = {"packages": [source, patch, {**source, "SPDXID": "SPDXRef-Bottle-qtbase", "versionInfo": "9.0.0"}],
                "relationships": [{"spdxElementId": patch["SPDXID"], "relationshipType": "PATCH_APPLIED", "relatedSpdxElement": source["SPDXID"]}]}
        (self.root / "sbom.spdx.json").write_text(json.dumps(sbom))
        (self.root / "INSTALL_RECEIPT.json").write_text(json.dumps({"source": {"versions": {"stable": "6.7.1"}}}))
        (self.root / ".brew").mkdir()
        (self.root / ".brew/qtbase.rb").write_text('mirror "https://mirror.invalid/qtbase-6.7.1.tar.xz"\n')
        version, sources = collector.source_records(self.root, "qtbase")
        self.assertEqual(version, "6.7.1")
        self.assertEqual([s["kind"] for s in sources], ["source", "patch"])
        self.assertEqual(sources[0]["file"], "qtbase/upstream.tar.xz")
        self.assertEqual(sources[1]["sha256"], "b" * 64)

    def test_source_package_rejects_modified_dependency_inputs(self):
        cache = self.root
        source = b"upstream source"
        checksum = hashlib.sha256(source).hexdigest()
        (cache / "downloads").mkdir()
        (cache / "downloads" / checksum).write_bytes(source)
        recipe = cache / "recipes/example"
        recipe.mkdir(parents=True)
        (recipe / "formula.rb").write_bytes(b"recipe")
        (recipe / "sbom.spdx.json").write_bytes(b"metadata")
        manifest = {"dependencies": [{"name": "example", "sources": [{"kind": "source", "file": "example/upstream.tar.gz", "sha256": checksum}],
                     "recipe_sha256": hashlib.sha256(b"recipe").hexdigest(), "sbom_sha256": hashlib.sha256(b"metadata").hexdigest()}]}
        self.assertEqual(len(packager.verified_inputs(manifest, cache)), 3)
        (recipe / "formula.rb").write_bytes(b"changed recipe")
        with self.assertRaisesRegex(RuntimeError, "changed source input"):
            packager.verified_inputs(manifest, cache)

    def test_package_validation_rejects_escaping_source_filename(self):
        manifest = {"dependencies": [{"name": "example", "sources": [{"kind": "source", "file": "../outside", "sha256": "a" * 64}]}]}
        with self.assertRaisesRegex(RuntimeError, "Invalid source filename"):
            packager.verified_inputs(manifest, self.root)

    def test_source_package_contains_committed_code_and_exact_dependency(self):
        project = self.root / "project"
        project.mkdir()
        def git(*args):
            return subprocess.check_output(["git", *args], cwd=project, text=True).strip()
        git("init", "--quiet")
        git("config", "user.name", "License fixture")
        git("config", "user.email", "fixture@example.invalid")
        (project / ".gitignore").write_text("build/\ndist/\n")
        (project / "LICENSE").write_bytes(b"fixture license")
        (project / "main.cpp").write_bytes(b"committed application source")
        git("add", ".")
        git("commit", "--quiet", "-m", "Fixture source")
        commit = git("rev-parse", "HEAD")
        app = project / "dist/ikview.app"
        notices = app / "Contents/Resources/third-party"
        notices.mkdir(parents=True)
        (app / "Contents/Resources/LICENSE").write_bytes(b"fixture license")
        (app / "Contents/MacOS").mkdir()
        (app / "Contents/MacOS/ikview").write_bytes(b"fixture executable")
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps({"CFBundleShortVersionString": "0.1.0", "CFBundleExecutable": "ikview"}))
        (notices / "Homebrew-LICENSE.txt").write_bytes(b"fixture recipe license")
        cache = project / "build/license-sources"
        (cache / "downloads").mkdir(parents=True)
        recipe = cache / "recipes/example"
        recipe.mkdir(parents=True)
        upstream = self.archive({"example/LICENSE": b"upstream license", "example/code.c": b"upstream code"}).read_bytes()
        checksum = hashlib.sha256(upstream).hexdigest()
        (cache / "downloads" / checksum).write_bytes(upstream)
        (recipe / "formula.rb").write_bytes(b"fixture build options")
        (recipe / "sbom.spdx.json").write_bytes(b"fixture dependency metadata")
        manifest = {"project_commit": commit, "project_dirty": False, "dependencies": [{"name": "example",
                    "sources": [{"kind": "source", "file": "example/upstream.tar.gz", "sha256": checksum}],
                    "recipe_sha256": packager.sha256(recipe / "formula.rb"), "sbom_sha256": packager.sha256(recipe / "sbom.spdx.json")}]}
        (notices / "source-manifest.json").write_text(json.dumps(manifest))
        with redirect_stdout(io.StringIO()):
            packager.package(project, app, project / "dist")
        with tarfile.open(project / "dist/ikview-0.1.0-source.tar.gz") as archive:
            self.assertEqual(archive.extractfile("ikview-0.1.0/ikview/main.cpp").read(), b"committed application source")
            self.assertEqual(archive.extractfile("ikview-0.1.0/dependencies/example/upstream.tar.gz").read(), upstream)
            self.assertTrue(all(m.uname == "" and m.uid == 0 for m in archive.getmembers()))
            self.assertFalse(any("/.git/" in m.name for m in archive.getmembers()))
        before = (project / "dist/ikview-0.1.0-source.tar.gz").read_bytes()
        (project / "main.cpp").write_bytes(b"uncommitted source change")
        with self.assertRaisesRegex(RuntimeError, "Commit the release"):
            packager.package(project, app, project / "dist")
        self.assertEqual((project / "dist/ikview-0.1.0-source.tar.gz").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
