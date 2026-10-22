#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
EastWind DX9 file tool.

Our DX9 files sit beside the DX8 files they replace (dx9wrapper.cpp next to dx8wrapper.cpp)
and are listed in EastWind/dx9_files.txt. Files that only the DX9 build has (no DX8 original)
are listed in EastWind/dx9_new_files.txt. Shared files include a replaced header through:

    #ifdef EW_RENDERER_DX9
    #include "dx9wrapper.h"
    #else
    #include "dx8wrapper.h"
    #endif

Commands:
    check                 (default) Run after every merge of main. Reports DX8 files changed
                          since their DX9 file was started, DX8 files that no longer use raw
                          D3D8 (the DX9 file can go), include sites that are not switched,
                          and drift between our presets and upstream's. Also checks that
                          only DX9 files include the DX9-only files.
    add <DX8 path>...     Start DX9 files: copy each DX8 file to its DX9 name, record the
                          upstream commit, and switch the includes in shared files.
    fix-includes          Switch every include of a listed DX8 header in shared files, and
                          point DX9 files straight at the DX9 headers.
    deps <build dir>      After a DX9 build: list compiled files that still pulled in a
                          replaced DX8 file.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

LIST_FILE = "EastWind/dx9_files.txt"
NEW_FILE = "EastWind/dx9_new_files.txt"
SHARED_FILE = "EastWind/dx9_shared.txt"
PRESET_FILE = "cmake/EastWindPresets.json"
SCAN_ROOTS = ("Core/", "GeneralsMD/")
SOURCE_EXTENSIONS = {".h", ".hpp", ".inl", ".c", ".cc", ".cpp", ".cxx"}
SWITCH = "EW_RENDERER_DX9"

# The same two-step scan the Phase 1 plan used: a file that mentions DX8 and uses raw D3D8 tokens.
# D3D8 shaders are DWORD handles (D3D9 uses interface pointers), and the LPDIRECT3D...8 typedefs name D3D8 interfaces.
MENTIONS_DX8 = re.compile(r"IDirect3D\w*8|LPDIRECT3D\w*8|\bDWORD\s+\w*Shader\b|D3D\w+8\b|DX8\w+|dx8\w*\.h|d3d8\.h|d3dx8\w*\.h|D3DX[A-Z]\w*\(|_Get_D3D_Device8")
RAW_D3D8 = re.compile(r"IDirect3D\w*8|LPDIRECT3D\w*8|\bDWORD\s+\w*Shader\b|D3D[A-Z_]*8\b|_Get_D3D_Device8|D3DX[A-Z]\w*|D3DRS_|D3DTSS_|D3DFMT_|D3DPOOL_|D3DUSAGE_|D3DFVF_|D3DPT_|D3DLOCK_")
# d3d8.h and d3d9.h cannot share a translation unit, so including a D3D8 SDK header is raw too.
SDK_D3D8_INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"](?:d3d8\w*|d3dx8\w*)\.h[>"]', re.IGNORECASE | re.MULTILINE)

INCLUDE = re.compile(r'^([ \t]*)#[ \t]*include[ \t]*(["<])([^">]+)[">]')
IFDEF_SWITCH = re.compile(r"^[ \t]*#[ \t]*ifdef[ \t]+" + SWITCH + r"\b")
ELSE = re.compile(r"^[ \t]*#[ \t]*else\b")
ENDIF = re.compile(r"^[ \t]*#[ \t]*endif\b")


def dx9_name(name):
    """dx8wrapper.cpp -> dx9wrapper.cpp, DX8Backend.cpp -> DX9Backend.cpp, texture.h -> texture_dx9.h"""
    if re.search("dx8", name, re.IGNORECASE):
        return re.sub("dx8", lambda m: m.group(0)[:-1] + "9", name, count=1, flags=re.IGNORECASE)
    stem, ext = os.path.splitext(name)
    return stem + "_dx9" + ext


def run_git(root, *args, check=True):
    result = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True)
    if check and result.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    return result


def read_text(path):
    # latin-1 keeps every byte, so files are written back unchanged apart from our edits.
    with open(path, "rb") as f:
        return f.read().decode("latin-1")


def write_text(path, text):
    with open(path, "wb") as f:
        f.write(text.encode("latin-1"))


# Comments and string literals, so a D3D8 name inside them (a memory pool name, a note) is not code.
COMMENT_OR_STRING = re.compile(r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*\'''', re.DOTALL)


def code_only(text):
    # Comments keep their line breaks; strings become "".
    return COMMENT_OR_STRING.sub(lambda m: "\n" * m.group(0).count("\n") if m.group(0).startswith("/") else '""', text)


def is_raw_d3d8(text):
    if SDK_D3D8_INCLUDE.search(text):
        return True
    return bool(MENTIONS_DX8.search(text)) and bool(RAW_D3D8.search(code_only(text)))


class Repo:
    def __init__(self, root):
        self.root = root
        files = run_git(root, "ls-files").stdout.splitlines()
        self.tracked = set(files)
        self.tracked_lower = {f.lower(): f for f in files}
        self.by_name = defaultdict(list)
        for f in files:
            if not f.startswith("Generals/"):
                self.by_name[os.path.basename(f).lower()].append(f)
        self.sources = sorted(
            f for f in files
            if f.startswith(SCAN_ROOTS) and os.path.splitext(f)[1].lower() in SOURCE_EXTENSIONS)
        self.entries = self._read_list()
        self.dx8_files = {e["dx8"] for e in self.entries}
        self.dx9_files = {e["dx9"] for e in self.entries}
        self.header_dx9 = {e["dx8"]: e["dx9"] for e in self.entries if e["dx8"].lower().endswith((".h", ".hpp", ".inl"))}
        self.dx9_to_dx8 = {e["dx9"]: e["dx8"] for e in self.entries}
        self.keep_shared = self._read_shared()
        self.new_entries = self._read_new_list()
        self.new_files = {e["path"] for e in self.new_entries}

    def is_raw(self, f, text=None):
        """Raw D3D8 code that needs a DX9 file, unless dx9_shared.txt says it may stay shared."""
        if f in self.keep_shared:
            return False
        return is_raw_d3d8(read_text(self.path(f)) if text is None else text)

    def _read_shared(self):
        shared = {}
        shared_path = self.path(SHARED_FILE)
        if not os.path.exists(shared_path):
            return shared
        for number, line in enumerate(read_text(shared_path).splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            path, _, reason = line.partition(" ")
            if not reason.strip():
                sys.exit(f"{SHARED_FILE}:{number}: expected '<repo path> <reason>'")
            shared[path] = number
        return shared

    def path(self, rel):
        return os.path.join(self.root, rel)

    def _read_list(self):
        entries = []
        list_path = self.path(LIST_FILE)
        if not os.path.exists(list_path):
            return entries
        for number, line in enumerate(read_text(list_path).splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) != 3:
                sys.exit(f"{LIST_FILE}:{number}: expected '<DX8 path> <DX9 name> <commit>'")
            dx8, name, commit = fields
            entries.append({"dx8": dx8, "dx9": os.path.dirname(dx8) + "/" + name, "commit": commit, "line": number})
        return entries

    def _read_new_list(self):
        entries = []
        list_path = self.path(NEW_FILE)
        if not os.path.exists(list_path):
            return entries
        for number, line in enumerate(read_text(list_path).splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) != 2:
                sys.exit(f"{NEW_FILE}:{number}: expected '<repo path of the new file> <repo path of its anchor DX9 file>'")
            entries.append({"path": fields[0], "anchor": fields[1], "line": number})
        return entries

    def resolve(self, including, text):
        """Repo path an include names, or None. Same folder first, then the nearest file of that name."""
        local = os.path.normpath(os.path.join(os.path.dirname(including), text)).replace("\\", "/")
        if local.lower() in self.tracked_lower:
            return self.tracked_lower[local.lower()]
        suffix = "/" + text.lower().lstrip("./")
        candidates = [f for f in self.by_name.get(os.path.basename(text).lower(), [])
                      if ("/" + f.lower()).endswith(suffix)]
        if not candidates:
            return None
        if len(candidates) == 1:
            return candidates[0]

        def shared(f):
            return len(os.path.commonprefix([f.split("/"), including.split("/")]))
        candidates.sort(key=shared, reverse=True)
        return candidates[0] if shared(candidates[0]) > shared(candidates[1]) else None

    def shared_files(self):
        """Files compiled in both builds: not a listed DX8, DX9 or DX9-only file, and not an unported raw D3D8 file."""
        for f in self.sources:
            if f in self.dx8_files or f in self.dx9_files or f in self.new_files:
                continue
            text = read_text(self.path(f))
            if self.is_raw(f, text):
                continue
            yield f, text


def switched_at(lines, i, repo, including, dx9):
    """True when lines[i] (a DX8 include) is the #else branch of a correct EW_RENDERER_DX9 block."""
    if i < 3 or i + 1 >= len(lines):
        return False
    m = INCLUDE.match(lines[i - 2])
    return (IFDEF_SWITCH.match(lines[i - 3]) and m and repo.resolve(including, m.group(3)) == dx9
            and ELSE.match(lines[i - 1]) and ENDIF.match(lines[i + 1])) is not None and bool(m)


def eol_of(line):
    return line[len(line.rstrip("\r\n")):] or "\n"


def replace_name(line, text, new_name):
    new_text = text[: len(text) - len(os.path.basename(text))] + new_name
    return line.replace(text, new_text, 1)


def scan_shared(repo, fix=False):
    """Checks (or fixes) include sites in shared files. Returns problem strings."""
    problems = []
    for f, text in repo.shared_files():
        lines = text.splitlines(keepends=True)
        out = []
        changed = False
        for i, line in enumerate(lines):
            m = INCLUDE.match(line)
            target = repo.resolve(f, m.group(3)) if m else None
            if target in repo.header_dx9:
                dx9 = repo.header_dx9[target]
                if not switched_at(lines, i, repo, f, dx9):
                    if fix:
                        eol = eol_of(line)
                        if not line.endswith(("\n", "\r")):
                            line += eol
                        indent = m.group(1)
                        out += [f"{indent}#ifdef {SWITCH}{eol}", replace_name(line, m.group(3), os.path.basename(dx9)),
                                f"{indent}#else{eol}", line, f"{indent}#endif{eol}"]
                        changed = True
                        continue
                    problems.append(f"{f}:{i + 1}: includes {target} without the {SWITCH} switch")
            elif target in repo.dx9_files:
                # Must be the first branch of a switch block, or the DX8 build breaks.
                ok = (i >= 1 and i + 2 < len(lines) and IFDEF_SWITCH.match(lines[i - 1]) and ELSE.match(lines[i + 1])
                      and INCLUDE.match(lines[i + 2])
                      and repo.resolve(f, INCLUDE.match(lines[i + 2]).group(3)) == repo.dx9_to_dx8[target])
                if not ok:
                    problems.append(f"{f}:{i + 1}: includes DX9 file {target} outside an {SWITCH} block")
            elif target in repo.new_files:
                # The DX8 build does not have the file at all, so it may only appear under the switch.
                if not (i >= 1 and IFDEF_SWITCH.match(lines[i - 1])):
                    problems.append(f"{f}:{i + 1}: includes DX9-only file {target} outside an {SWITCH} block")
            out.append(line)
        if changed:
            write_text(repo.path(f), "".join(out))
            print(f"  switched includes in {f}")
    return problems


def scan_dx9_files(repo, fix=False):
    """DX9 files (and DX9-only files) include DX9 headers directly."""
    problems = []
    for f in sorted(repo.dx9_files | repo.new_files):
        if not os.path.exists(repo.path(f)):
            continue
        lines = read_text(repo.path(f)).splitlines(keepends=True)
        changed = False
        for i, line in enumerate(lines):
            m = INCLUDE.match(line)
            target = repo.resolve(f, m.group(3)) if m else None
            if target in repo.header_dx9:
                if fix:
                    lines[i] = replace_name(line, m.group(3), os.path.basename(repo.header_dx9[target]))
                    changed = True
                else:
                    problems.append(f"{f}:{i + 1}: DX9 file includes DX8 header {target}")
        if changed:
            write_text(repo.path(f), "".join(lines))
            print(f"  pointed {f} at DX9 headers")
    return problems


def resolved_preset(presets, name):
    by_name = {p["name"]: p for p in presets}
    preset = by_name[name]
    merged = {}
    for parent in ([preset["inherits"]] if isinstance(preset.get("inherits"), str) else preset.get("inherits", [])):
        merged = resolved_preset(presets, parent)
    result = dict(merged)
    for key, value in preset.items():
        if key == "cacheVariables":
            result[key] = {**merged.get(key, {}), **value}
        else:
            result[key] = value
    return result


def check_presets(root):
    with open(os.path.join(root, "CMakePresets.json"), encoding="utf-8") as f:
        upstream = resolved_preset(json.load(f)["configurePresets"], "win32")
    with open(os.path.join(root, PRESET_FILE), encoding="utf-8") as f:
        ours = resolved_preset(json.load(f)["configurePresets"], "ew-win32-base")
    problems = []
    for key in ("generator", "binaryDir", "architecture", "vendor"):
        if upstream.get(key) != ours.get(key):
            problems.append(f"{PRESET_FILE}: '{key}' differs from upstream win32 ({upstream.get(key)!r})")
    ours_vars = {k: v for k, v in ours.get("cacheVariables", {}).items() if k != "RTS_BUILD_GENERALS"}
    if upstream.get("cacheVariables", {}) != ours_vars:
        problems.append(f"{PRESET_FILE}: cacheVariables differ from upstream win32 ({upstream.get('cacheVariables')!r})")
    return problems


def is_header(path):
    return path.lower().endswith((".h", ".hpp", ".inl"))


def includes_of(repo, f):
    found = []
    for line in read_text(repo.path(f)).splitlines():
        m = INCLUDE.match(line)
        target = repo.resolve(f, m.group(3)) if m else None
        if target:
            found.append(target)
    return found


def raw_unported(repo):
    return [f for f in repo.sources if f not in repo.dx8_files and f not in repo.dx9_files
            and f not in repo.new_files and repo.is_raw(f)]


def raw_dependents(repo, adding=()):
    """Raw D3D8 files without a DX9 file that include a replaced header (or one being added),
    followed through the raw headers they replace in turn. Shared files are switched, so only
    raw files can pull in both versions of a header. Returns {file: replaced header it includes}."""
    replaced = set(repo.header_dx9) | {p for p in adding if is_header(p)}
    pending = {f: includes_of(repo, f) for f in raw_unported(repo) if f not in adding}
    found = {}
    grew = True
    while grew:
        grew = False
        for f, targets in list(pending.items()):
            hit = next((t for t in targets if t in replaced), None)
            if hit:
                found[f] = hit
                del pending[f]
                if is_header(f):
                    replaced.add(f)
                grew = True
    return found


def cmd_check(repo, verbose):
    actions = []
    for e in repo.entries:
        where = f"{LIST_FILE}:{e['line']}"
        if not os.path.exists(repo.path(e["dx9"])):
            actions.append(f"{where}: {e['dx9']} is missing")
        if run_git(repo.root, "cat-file", "-e", e["commit"] + "^{commit}", check=False).returncode != 0:
            actions.append(f"{where}: unknown commit {e['commit']}")
            continue
        if e["dx8"] not in repo.tracked:
            actions.append(f"{where}: {e['dx8']} was removed or renamed upstream")
            continue
        if run_git(repo.root, "diff", "--quiet", e["commit"], "HEAD", "--", e["dx8"], check=False).returncode != 0:
            log = run_git(repo.root, "log", "--oneline", f"{e['commit']}..HEAD", "--", e["dx8"]).stdout.strip()
            actions.append(f"{where}: {e['dx8']} changed upstream; port it to {os.path.basename(e['dx9'])}, "
                           f"then set the commit to HEAD:\n      " + log.replace("\n", "\n      "))
        if not is_raw_d3d8(read_text(repo.path(e["dx8"]))):
            actions.append(f"{where}: {e['dx8']} no longer uses raw D3D8; the DX9 file can probably be deleted")

    for e in repo.new_entries:
        where = f"{NEW_FILE}:{e['line']}"
        if not os.path.exists(repo.path(e["path"])):
            actions.append(f"{where}: {e['path']} is missing")
        elif e["path"] not in repo.tracked:
            actions.append(f"{where}: {e['path']} is not tracked; git add it")
        if e["path"] in repo.dx8_files or e["path"] in repo.dx9_files:
            actions.append(f"{where}: {e['path']} is also in {LIST_FILE}; keep one of the two")
        if e["anchor"] not in repo.dx9_files:
            actions.append(f"{where}: anchor {e['anchor']} is not a DX9 file from {LIST_FILE}")

    for f, number in sorted(repo.keep_shared.items()):
        where = f"{SHARED_FILE}:{number}"
        if f not in repo.tracked:
            actions.append(f"{where}: {f} was removed or renamed upstream")
        elif f in repo.dx8_files:
            actions.append(f"{where}: {f} also has a DX9 file in {LIST_FILE}; keep one of the two")
        elif not is_raw_d3d8(read_text(repo.path(f))):
            actions.append(f"{where}: {f} no longer uses D3D8; remove the line")

    actions += scan_shared(repo)
    actions += scan_dx9_files(repo)
    for f, header in sorted(raw_dependents(repo).items()):
        actions.append(f"{f}: raw D3D8 file includes replaced header {header} but has no DX9 file; "
                       f"add it, or the DX9 build mixes both versions")
    actions += check_presets(repo.root)

    raw_left = raw_unported(repo)
    print(f"DX9 files: {len(repo.entries)}. DX9-only files: {len(repo.new_entries)}. "
          f"Raw D3D8 files without a DX9 file: {len(raw_left)}.")
    if verbose:
        for f in raw_left:
            print(f"  {f}")
    if actions:
        print(f"\n{len(actions)} item(s) need attention:")
        for a in actions:
            print(f"  {a}")
        return 1
    print("Everything is in sync.")
    return 0


def cmd_add(repo, paths, dry_run):
    head = run_git(repo.root, "rev-parse", "HEAD").stdout.strip()
    new_lines = []
    paths = [p.replace("\\", "/") for p in paths]
    # Raw files that include a header being replaced must move to DX9 in the same step.
    for f, header in sorted(raw_dependents(repo, paths).items()):
        print(f"also adding {f} (includes {os.path.basename(header)})")
        paths.append(f)
    for p in paths:
        if p not in repo.tracked:
            sys.exit(f"{p} is not a tracked file")
        if not p.startswith(SCAN_ROOTS):
            sys.exit(f"{p}: only Core/ and GeneralsMD/ files get DX9 versions")
        if p in repo.dx8_files:
            sys.exit(f"{p} already has a DX9 file")
        name = dx9_name(os.path.basename(p))
        dx9 = os.path.dirname(p) + "/" + name
        if os.path.exists(repo.path(dx9)):
            sys.exit(f"{dx9} already exists")
        print(f"{p} -> {name}")
        if dry_run:
            continue
        text = read_text(repo.path(p))
        if p.lower().endswith((".h", ".hpp", ".inl")):
            # A different include guard, so a TU that pulls in both versions fails to compile.
            text, n = re.subn(r"^([ \t]*#[ \t]*ifndef[ \t]+)(\w+)([ \t]*\r?\n[ \t]*#[ \t]*define[ \t]+)\2\b",
                              r"\1\2_DX9\3\2_DX9", text, count=1, flags=re.MULTILINE)
        write_text(repo.path(dx9), text)
        new_lines.append(f"{p} {name} {head}\n")
    if dry_run:
        return 0
    list_text = read_text(repo.path(LIST_FILE))
    if not list_text.endswith("\n"):
        list_text += "\n"
    write_text(repo.path(LIST_FILE), list_text + "".join(new_lines))
    return cmd_fix(Repo(repo.root))


def cmd_fix(repo):
    problems = scan_shared(repo, fix=True) + scan_dx9_files(repo, fix=True)
    for p in problems:
        print(f"  {p}")
    return 1 if problems else 0


def cmd_deps(repo, build_dir, config):
    build_dir = os.path.abspath(build_dir)
    ninja = "ninja"
    cache = os.path.join(build_dir, "CMakeCache.txt")
    if os.path.exists(cache):
        for line in read_text(cache).splitlines():
            if line.startswith("CMAKE_MAKE_PROGRAM:"):
                ninja = line.split("=", 1)[1]
    ninja_file = f"build-{config}.ninja"
    if not os.path.exists(os.path.join(build_dir, ninja_file)):
        ninja_file = "build.ninja"
    result = subprocess.run([ninja, "-C", build_dir, "-f", ninja_file, "-t", "deps"], capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"ninja -t deps failed: {result.stderr.strip()}")
    replaced = {os.path.normcase(os.path.abspath(repo.path(f))) for f in repo.dx8_files}
    leaks = defaultdict(set)
    target = None
    count = 0
    for line in result.stdout.splitlines():
        if not line.strip():
            continue
        if not line.startswith((" ", "\t")):
            target = line.split(":")[0]
            continue
        count += 1
        dep = os.path.normcase(os.path.normpath(os.path.join(build_dir, line.strip())))
        if dep in replaced:
            leaks[dep].add(target)
    if count == 0:
        sys.exit("No dependency information found. Build first.")
    if leaks:
        print(f"{len(leaks)} replaced DX8 file(s) are still used by the DX9 build:")
        for dep, targets in sorted(leaks.items()):
            print(f"  {dep}")
            for t in sorted(targets)[:5]:
                print(f"      {t}")
        return 1
    print(f"No replaced DX8 file is used ({count} dependencies checked, {len(repo.dx8_files)} replaced files).")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", help="repository root (default: this checkout)")
    sub = parser.add_subparsers(dest="command")
    check = sub.add_parser("check")
    check.add_argument("-v", "--verbose", action="store_true", help="list raw D3D8 files without a DX9 file")
    add = sub.add_parser("add")
    add.add_argument("paths", nargs="+")
    add.add_argument("-n", "--dry-run", action="store_true")
    sub.add_parser("fix-includes")
    deps = sub.add_parser("deps")
    deps.add_argument("build_dir")
    deps.add_argument("--config", default="Release")
    args = parser.parse_args()

    root = args.root or run_git(os.path.dirname(os.path.abspath(__file__)), "rev-parse", "--show-toplevel").stdout.strip()
    repo = Repo(root)
    if args.command == "add":
        return cmd_add(repo, args.paths, args.dry_run)
    if args.command == "fix-includes":
        return cmd_fix(repo)
    if args.command == "deps":
        return cmd_deps(repo, args.build_dir, args.config)
    return cmd_check(repo, getattr(args, "verbose", False))


if __name__ == "__main__":
    sys.exit(main())
