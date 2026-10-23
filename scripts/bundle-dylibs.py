#!/usr/bin/env python3
"""Bundle dylibs and verify that every non-system import resolves inside the app."""
import argparse
import os
import pathlib
import plistlib
import re
import shutil
import subprocess


def run(*args):
    return subprocess.check_output(args, text=True)


def inside(path, directory):
    return path.resolve().is_relative_to(directory.resolve())


def system_path(path):
    return any(path.is_relative_to(root) for root in (pathlib.Path("/System/Library"), pathlib.Path("/usr/lib")))


def macho(path):
    return path.is_file() and not path.is_symlink() and "Mach-O" in run("file", "-b", str(path))


def load_commands(binary):
    dependencies, rpaths = [], []
    for block in re.split(r"(?m)^Load command \d+\s*$", run("otool", "-l", str(binary)))[1:]:
        command = re.search(r"(?m)^\s*cmd (\S+)", block)
        if not command:
            continue
        kind = command.group(1)
        if kind == "LC_RPATH":
            match = re.search(r"(?m)^\s*path (.+) \(offset \d+\)", block)
            if match:
                rpaths.append(match.group(1))
        elif kind in {"LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB"}:
            match = re.search(r"(?m)^\s*name (.+) \(offset \d+\)", block)
            if match:
                dependencies.append(match.group(1))
    # LC_ID_DYLIB is an identity, not an imported library.
    return list(dict.fromkeys(dependencies)), list(dict.fromkeys(rpaths))


def executable_path(app):
    with (app / "Contents/Info.plist").open("rb") as source:
        name = plistlib.load(source)["CFBundleExecutable"]
    executable = (app / "Contents/MacOS" / name).resolve()
    if not inside(executable, app) or not executable.is_file():
        raise RuntimeError(f"Missing bundled executable: {executable}")
    return executable


def reference_paths(reference, loader, executable, rpaths):
    for token, base in (("@loader_path", loader.parent), ("@executable_path", executable.parent)):
        if reference == token or reference.startswith(token + "/"):
            return [(base / reference[len(token):].lstrip("/")).resolve()]
    if reference == "@rpath" or reference.startswith("@rpath/"):
        suffix = reference[len("@rpath"):].lstrip("/")
        return [(directory / suffix).resolve() for directory in rpaths]
    if pathlib.Path(reference).is_absolute():
        # System libraries may exist only in the dyld shared cache.
        return [pathlib.Path(os.path.normpath(reference))]
    raise RuntimeError(f"Unsupported relative load path in {loader}: {reference}")


def run_paths(binary, executable, inherited):
    _, declared = load_commands(binary)
    paths = []
    for reference in declared:
        paths.extend(reference_paths(reference, binary, executable, inherited))
    return tuple(dict.fromkeys([*paths, *inherited]))


def resolve_import(reference, binary, executable, rpaths):
    candidates = reference_paths(reference, binary, executable, rpaths)
    for candidate in candidates:
        if system_path(candidate) or candidate.is_file():
            return candidate
    return None


def verify_bundle(app):
    """Read-only dyld-style traversal, including dynamically loaded plugin roots."""
    app = app.resolve()
    executable = executable_path(app)
    binaries = {path.resolve() for path in app.rglob("*") if macho(path)}
    if executable not in binaries:
        raise RuntimeError(f"Executable is not Mach-O: {executable}")
    executable_rpaths = run_paths(executable, executable, ())
    queue = [(executable, ())]
    # Plugins are dlopened by the executable, so they inherit its run-paths.
    queue.extend((binary, executable_rpaths) for binary in binaries
                 if any(part.lower() == "plugins" for part in binary.relative_to(app).parts[:-1]))
    visited = set()
    inspected = set()
    while queue or binaries - inspected:
        # Follow real loader chains first. A framework can legitimately rely
        # on its parent library's LC_RPATH instead of declaring its own.
        if not queue:
            queue.append((next(iter(binaries - inspected)), executable_rpaths))
        binary, inherited = queue.pop()
        search = run_paths(binary, executable, inherited)
        state = (binary, search)
        if state in visited:
            continue
        visited.add(state)
        inspected.add(binary)
        dependencies, _ = load_commands(binary)
        for reference in dependencies:
            resolved = resolve_import(reference, binary, executable, search)
            if resolved is None:
                attempted = reference_paths(reference, binary, executable, search)
                raise RuntimeError(f"Unresolved dependency in {binary}: {reference}; searched {attempted}")
            if system_path(resolved):
                continue
            if pathlib.Path(reference).is_absolute():
                raise RuntimeError(f"Nonportable absolute dependency in {binary}: {reference}")
            if not inside(resolved, app) or resolved.resolve() not in binaries:
                raise RuntimeError(f"Dependency escapes bundle or is not Mach-O in {binary}: {reference} -> {resolved}")
            queue.append((resolved.resolve(), search))
    print(f"Verified {len(binaries)} bundled Mach-O binaries and their resolved imports")


def bundle(app):
    app = app.resolve()
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(exist_ok=True)
    executable = executable_path(app)
    initial_rpaths = run_paths(executable, executable, ())
    queue = [(path.resolve(), path.resolve(), initial_rpaths) for path in app.rglob("*") if macho(path)]
    seen = set()
    while queue:
        binary, origin, inherited = queue.pop()
        if binary in seen:
            continue
        seen.add(binary)
        framework_root = next((parent for parent in binary.parents if parent.name.endswith(".framework")), None)
        if framework_root:
            identity = "@rpath/" + str(binary.relative_to(framework_root.parent))
            subprocess.check_call(["install_name_tool", "-id", identity, str(binary)])
        elif binary.suffix == ".dylib":
            subprocess.check_call(["install_name_tool", "-id", "@rpath/" + binary.name, str(binary)])
        dependencies, declared_rpaths = load_commands(binary)
        search = tuple(dict.fromkeys([frameworks, *run_paths(binary, executable, inherited),
                                     *run_paths(origin, executable, inherited)]))
        for reference in dependencies:
            source = resolve_import(reference, binary, executable, search)
            if source is None and origin != binary:
                source = resolve_import(reference, origin, executable, search)
            # Preserve the Homebrew fallback used for frameworks omitted by macdeployqt.
            if source is None and reference.startswith("@rpath/"):
                for prefix in ("/opt/homebrew/lib", "/usr/local/lib"):
                    candidate = pathlib.Path(prefix) / reference.removeprefix("@rpath/")
                    if candidate.is_file():
                        source = candidate.resolve()
                        break
            if source is None:
                raise RuntimeError(f"Cannot locate dependency for bundling in {binary}: {reference}")
            if system_path(source):
                continue
            if inside(source, app):
                if pathlib.Path(reference).is_absolute():
                    replacement = "@loader_path/" + os.path.relpath(source, binary.parent)
                    subprocess.check_call(["install_name_tool", "-change", reference, replacement, str(binary)])
                continue
            source = source.resolve()
            source_framework = next((parent for parent in source.parents if parent.name.endswith(".framework")), None)
            if source_framework:
                target_root = frameworks / source_framework.name
                if not target_root.exists():
                    shutil.copytree(source_framework, target_root, symlinks=True)
                    queue.extend((path.resolve(), source_framework / path.relative_to(target_root), search)
                                 for path in target_root.rglob("*") if macho(path))
                target = target_root / source.relative_to(source_framework)
                replacement = "@rpath/" + str(target.relative_to(frameworks))
            else:
                target = frameworks / source.name
                if not target.exists():
                    shutil.copy2(source, target)
                    target.chmod(0o755)
                    queue.append((target.resolve(), source, search))
                replacement = "@rpath/" + target.name
            if not target.is_file():
                raise RuntimeError(f"Incomplete framework copied for {source}: missing {target}")
            subprocess.check_call(["install_name_tool", "-change", reference, replacement, str(binary)])
        for old in declared_rpaths:
            resolved_paths = reference_paths(old, binary, executable, inherited)
            external = any(not inside(path, app) and not system_path(path) for path in resolved_paths)
            absolute = pathlib.Path(old).is_absolute() and not system_path(pathlib.Path(old))
            if external or absolute:
                subprocess.check_call(["install_name_tool", "-delete_rpath", old, str(binary)])
        rpath = "@loader_path/" + os.path.relpath(frameworks, binary.parent)
        _, remaining_rpaths = load_commands(binary)
        if rpath not in remaining_rpaths:
            subprocess.check_call(["install_name_tool", "-add_rpath", rpath, str(binary)])
    verify_bundle(app)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=pathlib.Path)
    parser.add_argument("--verify-only", action="store_true", help="Inspect dependencies without modifying the bundle.")
    arguments = parser.parse_args()
    verify_bundle(arguments.app) if arguments.verify_only else bundle(arguments.app)
