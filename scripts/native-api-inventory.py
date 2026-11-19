#!/usr/bin/env python3
"""Compare RAVEN_API_INVENTORY snapshots without counting unimplemented provider stubs as APIs.

Host means a registered ApiRegistry handler; guest means a component export or a
provider forwarder to one. Dedicated dispatchers, guest fast paths, and COM vtable
methods are outside these counts. Reachability does not establish ABI correctness
or distinguish a component's implemented functions from its upstream stubs.
"""
import argparse
import json
import struct
from pathlib import Path


def component_exports(path):
    """Read named PE exports directly; no host objdump or guest execution is needed."""
    data = path.read_bytes()
    word = lambda at: struct.unpack_from("<H", data, at)[0]
    dword = lambda at: struct.unpack_from("<I", data, at)[0]
    pe = dword(0x3c)
    optional = pe + 24
    directory = optional + (112 if word(optional) == 0x20b else 96)
    export_rva = dword(directory)
    if not export_rva:
        return {}
    sections = optional + word(pe + 20)
    def offset(rva):
        for index in range(word(pe + 6)):
            at = sections + index * 40
            size, start, raw_size, raw = struct.unpack_from("<IIII", data, at + 8)
            if start <= rva < start + max(size, raw_size):
                return raw + rva - start
        raise ValueError(f"{path}: unmapped RVA {rva:x}")
    def text(rva):
        at = offset(rva)
        return data[at:data.index(0, at)].decode("ascii")
    table = offset(export_rva)
    names = offset(dword(table + 32))
    ordinals = offset(dword(table + 36))
    functions = offset(dword(table + 28))
    result, named = {}, set()
    for index in range(dword(table + 24)):
        ordinal = word(ordinals + index * 2)
        named.add(ordinal)
        address = dword(functions + ordinal * 4)
        if address:
            result[text(dword(names + index * 4))] = ("guest", "")
    for index in range(dword(table + 20)):
        if index not in named and dword(functions + index * 4):
            result["#" + str(dword(table + 16) + index)] = ("guest", "")
    return result


def inventory(path, component_directories=()):
    hosts, modules = {}, {}
    for line in path.read_text().splitlines():
        row = line.split("\t")
        if row[0] == "host":
            hosts[row[1]] = int(row[2])
        elif row[0] in ("guest", "provider"):
            modules.setdefault(row[1], {})[row[2]] = (row[0], row[3])
    if component_directories:
        aliases = {"raven-dwrite.dll": "dwrite.dll", "raven-d2d1.dll": "d2d1.dll", "raven-uianimation.dll": "uianimation.dll",
                   "ucrtbase32.dll": "ucrtbase.dll"}
        for path in sorted(path for directory in component_directories for path in directory.glob("*.dll")):
            module = aliases.get(path.name, path.name)
            modules.setdefault(module, component_exports(path))
    result = {}
    for module, exports in modules.items():
        reachable = {}
        for name, (kind, forwarder) in exports.items():
            if kind == "guest":
                reachable[name] = "guest"
            elif forwarder:
                target, symbol = forwarder.rsplit(".", 1)
                target = target.lower() + ("" if target.lower().endswith(".dll") else ".dll")
                if symbol in modules.get(target, {}):
                    reachable[name] = "guest"
            elif name in hosts:
                reachable[name] = "host"
        if reachable:
            result[module] = reachable
    return {"hosts": hosts, "modules": result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("x64", type=Path)
    parser.add_argument("x86", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--components64", type=Path, action="append", default=[])
    parser.add_argument("--components32", type=Path, action="append", default=[])
    args = parser.parse_args()
    wide, narrow = inventory(args.x64, args.components64), inventory(args.x86, args.components32)
    report = {}
    print("| DLL | x64 host / guest | x86 host / guest | x64 names absent from x86 |")
    print("|---|---:|---:|---:|")
    for module in sorted(wide["modules"].keys() | narrow["modules"].keys()):
        x64, x86 = wide["modules"].get(module, {}), narrow["modules"].get(module, {})
        missing = sorted(x64.keys() - x86.keys())
        counts = lambda names: f'{sum(v == "host" for v in names.values())} / {sum(v == "guest" for v in names.values())}'
        print(f"| {module} | {counts(x64)} | {counts(x86)} | {len(missing)} |")
        report[module] = {"x64": x64, "x86": x86, "missing": missing}
    if args.json:
        args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
