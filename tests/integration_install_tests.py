#!/usr/bin/env python3
"""Exercise installer/launcher contracts in disposable directories.

The tiny signed test app records arguments; its --register never touches Launch
Services or defaults. Launcher tests substitute only /usr/bin/open in a copy of
the shipped launcher with the same recorder, so no Finder/app UI is launched.
"""
import os
import pathlib
import plistlib
import shutil
import shlex
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "assets/integration/install.sh"
LAUNCHER = ROOT / "assets/integration/ikview"
FINDER_HELPER = ROOT / "assets/integration/finder-integration"


@unittest.skipUnless(sys.platform == "darwin", "macOS installer")
class IntegrationInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.shared = tempfile.TemporaryDirectory(prefix="ikview-integration-native-")
        cls.shared_path = pathlib.Path(cls.shared.name).resolve()
        source = cls.shared_path / "recorder.c"
        source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *path = getenv("IKVIEW_TEST_TRACE");
    if (path) {
        FILE *out = fopen(path, "wb"); if (!out) return 71;
        for (int i=0; i<argc; ++i) fwrite(argv[i], 1, strlen(argv[i])+1, out);
        fclose(out);
    }
    if (argc>1 && strcmp(argv[1], "--register")==0 && getenv("IKVIEW_TEST_FAIL_REGISTER")) return 37;
    puts("{}"); return 0;
}
''')
        cls.recorder = cls.shared_path / "recorder"
        subprocess.run(["xcrun", "clang", str(source), "-o", str(cls.recorder)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.shared.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ikview integration 한글 '")
        self.base = pathlib.Path(self.temp.name).resolve()
        self.prefix = self.base / "local prefix"
        self.destination = self.base / "Applications" / "IKView user's $(literal).app"
        self.source = self.base / "source space '" / "IKView.app"
        self.trace = self.base / "trace"
        self.env = dict(os.environ, IKVIEW_TEST_TRACE=str(self.trace))
        self.make_app(self.source, "new")

    def tearDown(self):
        self.temp.cleanup()

    def make_app(self, path, marker, identifier="io.ikview.desktop"):
        binary = path / "Contents/MacOS/ikview"
        binary.parent.mkdir(parents=True)
        shutil.copy2(self.recorder, binary)
        info = {"CFBundleIdentifier": identifier, "CFBundleExecutable": "ikview",
                "CFBundleName": "ikview", "CFBundlePackageType": "APPL", "CFBundleVersion": "1"}
        (path / "Contents/Info.plist").write_bytes(plistlib.dumps(info))
        (path / "Contents/Resources").mkdir()
        (path / "Contents/Resources/marker").write_text(marker)
        subprocess.run(["codesign", "--force", "--sign", "-", str(path)], check=True, capture_output=True)

    @property
    def launcher(self):
        return self.prefix / "bin/ikview"

    @property
    def config(self):
        return self.prefix / "share/ikview/app-path"

    def install(self, *extra, env=None):
        return subprocess.run(["/bin/sh", str(INSTALLER), "--app", str(self.source),
                               "--prefix", str(self.prefix), "--destination", str(self.destination), *extra],
                              capture_output=True, text=True, env=env or self.env, timeout=30)

    def install_existing_launcher(self, app_path=None):
        self.launcher.parent.mkdir(parents=True, exist_ok=True)
        self.config.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(LAUNCHER, self.launcher)
        self.launcher.chmod(0o755)
        self.config.write_text(str(app_path or self.destination) + "\n")

    def trace_arguments(self):
        return [x.decode() for x in self.trace.read_bytes().split(b"\0")[:-1]]

    def prepare_recorded_launcher(self):
        self.install_existing_launcher(self.source)
        # This one dependency substitution does not change parsing/path logic.
        opener = self.base / "native-open-recorder"
        shutil.copy2(self.recorder, opener)
        text = LAUNCHER.read_text()
        self.assertEqual(text.count("/usr/bin/open"), 1)
        self.launcher.write_text(text.replace("/usr/bin/open", '"' + str(opener) + '"'))
        self.launcher.chmod(0o755)

    def test_install_copies_signed_app_and_registers_exact_destination(self):
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.destination / "Contents/Resources/marker").read_text(), "new")
        self.assertEqual(self.config.read_text(), str(self.destination) + "\n")
        self.assertTrue(os.access(self.launcher, os.X_OK))
        self.assertEqual(self.trace_arguments(), [str(self.destination / "Contents/MacOS/ikview"), "--register"])
        self.assertFalse((self.base / "literal").exists())

    def test_foreign_command_and_symlinks_are_preserved(self):
        self.launcher.parent.mkdir(parents=True)
        self.launcher.write_text("#!/bin/sh\necho existing user command\n")
        before = self.launcher.read_bytes()
        result = self.install()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.launcher.read_bytes(), before)
        self.assertFalse(self.destination.exists())
        self.launcher.unlink()
        target = self.base / "unrelated"
        target.write_text("preserve me")
        self.launcher.symlink_to(target)
        result = self.install()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(target.read_text(), "preserve me")
        self.assertTrue(self.launcher.is_symlink())

    def test_foreign_destination_app_is_preserved(self):
        self.make_app(self.destination, "other", "example.other.app")
        result = self.install()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.destination / "Contents/Resources/marker").read_text(), "other")
        self.assertFalse(self.launcher.exists())

    def test_orphan_configuration_is_preserved(self):
        self.config.parent.mkdir(parents=True)
        self.config.write_text("preserve existing configuration\n")
        result = self.install()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.config.read_text(), "preserve existing configuration\n")
        self.assertFalse(self.destination.exists())

    def test_registration_failure_restores_previous_app_and_cli(self):
        self.make_app(self.destination, "old")
        self.install_existing_launcher()
        old_launcher, old_config = self.launcher.read_bytes(), self.config.read_bytes()
        result = self.install(env=dict(self.env, IKVIEW_TEST_FAIL_REGISTER="1"))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.destination / "Contents/Resources/marker").read_text(), "old")
        self.assertEqual(self.launcher.read_bytes(), old_launcher)
        self.assertEqual(self.config.read_bytes(), old_config)

    def test_cli_publish_failure_restores_both_launcher_and_configuration(self):
        previous_app = self.base / "previous location.app"
        self.make_app(previous_app, "previous destination")
        self.install_existing_launcher(previous_app)
        old_launcher, old_config = self.launcher.read_bytes(), self.config.read_bytes()
        fault_tools = self.base / "fault-tools"
        fault_tools.mkdir()
        fault = fault_tools / "mv"
        fault.write_text("#!/bin/sh\nfor task_arg in \"$@\"; do task_last=$task_arg; done\n"
                         'if [ "$task_last" = "$IKVIEW_TEST_FAIL_DEST" ]; then exit 76; fi\n'
                         'exec /bin/mv "$@"\n')
        fault.chmod(0o755)
        env = dict(self.env, PATH=str(fault_tools) + os.pathsep + self.env.get("PATH", ""),
                   IKVIEW_TEST_FAIL_DEST=str(self.launcher))
        result = self.install(env=env)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.destination.exists())
        self.assertEqual(self.launcher.read_bytes(), old_launcher)
        self.assertEqual(self.config.read_bytes(), old_config)
        self.assertEqual((previous_app / "Contents/Resources/marker").read_text(), "previous destination")

    def test_launcher_preserves_file_arguments_and_shell_syntax(self):
        self.prepare_recorded_launcher()
        args = ["photo space.png", "한글' $(touch injected).png", "line\nbreak.png", "--set-default"]
        result = subprocess.run([str(self.launcher), *args], cwd=self.base, env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.trace_arguments()[1:], ["-a", str(self.source), "--", *args])
        self.assertFalse((self.base / "injected").exists())
        result = subprocess.run([str(self.launcher), "--", "-filename.png"], cwd=self.base, env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.trace_arguments()[1:], ["-a", str(self.source), "--", "-filename.png"])

    def test_launcher_current_directory_and_management_routing(self):
        self.prepare_recorded_launcher()
        result = subprocess.run([str(self.launcher)], cwd=self.base, env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.trace_arguments()[1:], ["-a", str(self.source), "--", str(self.base)])
        args = ["--default-status", "jpg,png", "--defaults-backup", "backup space.json"]
        result = subprocess.run([str(self.launcher), *args], cwd=self.base, env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(self.trace_arguments(), [str(self.source / "Contents/MacOS/ikview"), *args])

    def test_launcher_routes_finder_management_to_bundled_helper(self):
        self.install_existing_launcher(self.source)
        helper = self.source / "Contents/Resources/integration/finder-integration"
        helper.parent.mkdir()
        helper.write_text('#!/bin/sh\nexec ' + shlex.quote(str(self.recorder)) + ' "$@"\n')
        for command, mode in (("--finder-status", "--status"), ("--finder-enable", "--enable"),
                              ("--finder-disable", "--disable")):
            result = subprocess.run([str(self.launcher), command], env=self.env, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(self.trace_arguments()[1:], ["--app", str(self.source), mode])
        result = subprocess.run([str(self.launcher), "--finder-enable", "unwanted file"],
                                env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 64)

    def prepare_finder_helper(self, election="+", running=False, fail_command=None):
        binary = self.source / "Contents/PlugIns/IKViewFinder.appex/Contents/MacOS/IKViewFinder"
        binary.parent.mkdir(parents=True)
        shutil.copy2(self.recorder, binary)
        extension = binary.parents[2]
        tools = self.base / "finder test tools"
        tools.mkdir()
        state = self.base / "plugin state"
        state.write_text(election + "    io.ikview.desktop.finder(0.6.0)\tUUID\t" + str(extension) + "\n")
        trace = self.base / "finder commands"
        pluginkit = tools / "pluginkit"
        pluginkit.write_text('''#!/bin/sh
printf '%s\\n' "$*" >> "$IKVIEW_HELPER_TRACE"
if [ "${IKVIEW_HELPER_FAIL-}" = "$1" ]; then exit 41; fi
if [ "$1" = -m ]; then cat "$IKVIEW_HELPER_STATE"; fi
''')
        codesign = tools / "codesign"
        codesign.write_text("#!/bin/sh\nif [ \"$1\" = -dvv ]; then printf '%s\\n' 'Signature=adhoc' >&2; fi\n")
        pgrep = tools / "pgrep"
        pgrep.write_text("#!/bin/sh\nexit " + ("0" if running else "1") + "\n")
        script = FINDER_HELPER.read_text()
        for name, tool in (("pluginkit", pluginkit), ("codesign", codesign), ("pgrep", pgrep)):
            tool.chmod(0o755)
            script = script.replace("/usr/bin/" + name, shlex.quote(str(tool)))
        helper = self.base / "finder integration under test"
        helper.write_text(script)
        env = dict(self.env, IKVIEW_HELPER_STATE=str(state), IKVIEW_HELPER_TRACE=str(trace))
        if fail_command:
            env["IKVIEW_HELPER_FAIL"] = fail_command
        return helper, env, trace, state

    def test_finder_activation_reports_election_separately_from_runtime(self):
        helper, env, trace, _ = self.prepare_finder_helper()
        result = subprocess.run(["/bin/sh", str(helper), "--app", str(self.source), "--enable"],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Registered at installed path: yes", result.stdout)
        self.assertIn("User election: use", result.stdout)
        self.assertIn("Extension process running: no", result.stdout)
        self.assertIn("Finder has not loaded", result.stdout)
        self.assertIn("Signing: ad hoc local build", result.stdout)
        commands = trace.read_text().splitlines()
        self.assertTrue(commands[0].startswith("-a "))
        self.assertEqual(commands[1], "-e use -i io.ikview.desktop.finder")

    def test_finder_status_does_not_register_or_change_preferences(self):
        helper, env, trace, _ = self.prepare_finder_helper(running=True)
        result = subprocess.run(["/bin/sh", str(helper), "--app", str(self.source), "--status"],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Extension process running: yes", result.stdout)
        self.assertEqual(trace.read_text().splitlines(), ["-m -v -i io.ikview.desktop.finder"])

    def test_finder_does_not_claim_another_install_path_is_registered(self):
        helper, env, _, state = self.prepare_finder_helper()
        state.write_text("+    io.ikview.desktop.finder(0.6.0)\t/Applications/Other.app/Contents/PlugIns/IKViewFinder.appex\n")
        result = subprocess.run(["/bin/sh", str(helper), "--app", str(self.source), "--status"],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("Registered at installed path: no", result.stdout)

    def test_finder_registration_failure_is_returned_without_false_success(self):
        helper, env, trace, _ = self.prepare_finder_helper(fail_command="-a")
        result = subprocess.run(["/bin/sh", str(helper), "--app", str(self.source), "--enable"],
                                env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 41)
        self.assertEqual(len(trace.read_text().splitlines()), 1)
        self.assertNotIn("User election: use", result.stdout)

    def test_rejects_newline_install_paths_without_creating_app(self):
        result = self.install("--destination", str(self.base / "new\nline.app"))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.destination.exists())
        self.assertFalse(self.launcher.exists())

    def test_remove_cli_keeps_app(self):
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.install("--remove-cli")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.launcher.exists())
        self.assertFalse(self.config.exists())
        self.assertTrue(self.destination.exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
