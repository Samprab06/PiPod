"""Post-generation checks for projects produced by :mod:`kicadgen`.

The helpers shell out to ``kicad-cli`` where needed and parse the results:

* :func:`run_erc` - run ERC and group violations by type.
* :func:`check_netlist` - export a netlist and compare it with the
  project's declared pin-to-net mapping.
* :func:`check_footprints` - compare symbol pin numbers with footprint pads.

Temporary reports are written to the system temp directory and removed
afterwards.  ``kicad-cli`` itself may create its usual ``.kicad_prl`` sidecar
in the project directory; that file is not produced by the generator.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from kicadgen import (  # noqa: E402
    KICAD_CLI,
    KICAD_FOOTPRINT_DIR,
    children,
    find_child,
    parse_sexpr_one,
)


def _cli(kicad_cli=None) -> str:
    if kicad_cli:
        return str(kicad_cli)
    return str(KICAD_CLI)


def find_root_schematic(project_dir) -> Path:
    """Locate the root schematic of a generated project directory."""
    directory = Path(project_dir)
    preferred = directory / f"{directory.name}.kicad_sch"
    if preferred.is_file():
        return preferred
    candidates = sorted(directory.glob("*.kicad_sch"))
    for candidate in candidates:
        try:
            root = parse_sexpr_one(candidate.read_text(encoding="utf-8"))
        except Exception:
            continue
        if find_child(root, "sheet_instances") is not None:
            return candidate
    if candidates:
        return candidates[0]
    raise FileNotFoundError(f"no .kicad_sch file found in {directory}")


def run_erc(project_dir, kicad_cli=None) -> dict[str, list]:
    """Run ``kicad-cli sch erc`` and return violations grouped by type."""
    root = find_root_schematic(project_dir)
    command = [
        _cli(kicad_cli), "sch", "erc", "--format", "json", "--severity-all",
    ]
    handle, report_path = tempfile.mkstemp(prefix="kicadgen-erc-", suffix=".json")
    os.close(handle)
    try:
        command += ["--output", report_path, str(root)]
        proc = subprocess.run(command, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(
                "kicad-cli sch erc failed:\n"
                + " ".join(command)
                + f"\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
        report = json.loads(Path(report_path).read_text(encoding="utf-8"))
    finally:
        try:
            os.unlink(report_path)
        except OSError:
            pass

    grouped: dict[str, list] = {}
    for sheet in report.get("sheets", []):
        sheet_path = sheet.get("path", "")
        for violation in sheet.get("violations", []):
            record = dict(violation)
            record["sheet"] = sheet_path
            grouped.setdefault(record.get("type", "unknown"), []).append(record)
    return grouped


def _strip_sheet_path(name: str, sheet_names: list[str]) -> str:
    """Remove a KiCad local-net sheet path prefix, e.g. '/MCU/GPIO4'."""
    for sheet_name in sorted(sheet_names, key=len, reverse=True):
        prefix = "/" + sheet_name + "/"
        if name.startswith(prefix):
            return name[len(prefix):]
    # A local net on the root sheet is exported as "/NET" (and a label that
    # itself contains '/' as "/A/B"); strip only the leading root marker.
    if name.startswith("/"):
        return name[1:]
    return name


def check_netlist(project, kicad_cli=None) -> dict:
    """Compare the exported netlist with the project's declared net list.

    Returns a dict with ``ok``, ``missing`` and ``extra`` lists.  Each entry
    is ``{ref, pin, expected, actual}``.
    """
    root = project.root_schematic_path
    if not root.is_file():
        raise FileNotFoundError(f"root schematic not written yet: {root}")
    command = [
        _cli(kicad_cli), "sch", "export", "netlist", "--format", "kicadsexpr",
    ]
    handle, netlist_path = tempfile.mkstemp(prefix="kicadgen-net-", suffix=".net")
    os.close(handle)
    try:
        command += ["--output", netlist_path, str(root)]
        proc = subprocess.run(command, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(
                "kicad-cli sch export netlist failed:\n"
                + " ".join(command)
                + f"\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
        netlist = parse_sexpr_one(Path(netlist_path).read_text(encoding="utf-8"))
    finally:
        try:
            os.unlink(netlist_path)
        except OSError:
            pass

    sheet_names = [sheet.name for sheet in project.sheets]
    nets_node = find_child(netlist, "nets")
    actual: dict[tuple[str, str], str] = {}
    net_names: list[str] = []
    if nets_node is not None:
        for net in children(nets_node, "net"):
            name_node = find_child(net, "name")
            net_name = str(name_node[1]) if name_node else ""
            net_names.append(net_name)
            for node in children(net, "node"):
                ref_node = find_child(node, "ref")
                pin_node = find_child(node, "pin")
                if ref_node is None or pin_node is None:
                    continue
                actual[(str(ref_node[1]), str(pin_node[1]))] = net_name

    expected: dict[tuple[str, str], str | None] = {}
    project_refs: set[str] = set()
    for part in project.iter_parts():
        project_refs.add(part.ref)
        for number in part.pin_numbers:
            if number in part.nets:
                expected[(part.ref, number)] = part.nets[number]
            else:
                expected[(part.ref, number)] = None

    def _normalise(raw):
        if raw is None:
            return None
        # KiCad exports pins without a connection as "unconnected-(...)".
        if raw.startswith("unconnected-"):
            return None
        return _strip_sheet_path(raw, sheet_names)

    missing: list[dict] = []
    extra: list[dict] = []
    for (ref, pin), wanted in expected.items():
        got = _normalise(actual.get((ref, pin)))
        if wanted is None:
            if got is not None:
                extra.append({"ref": ref, "pin": pin, "expected": None,
                              "actual": got})
            continue
        if got is None:
            missing.append({"ref": ref, "pin": pin, "expected": wanted,
                            "actual": None})
        elif got != wanted:
            missing.append({"ref": ref, "pin": pin, "expected": wanted,
                            "actual": got})
            extra.append({"ref": ref, "pin": pin, "expected": wanted,
                          "actual": got})

    # Nodes on project parts that were never declared (defensive).
    for (ref, pin), raw_name in actual.items():
        if ref in project_refs and (ref, pin) not in expected:
            extra.append({"ref": ref, "pin": pin, "expected": None,
                          "actual": _normalise(raw_name)})

    return {
        "ok": not missing and not extra,
        "missing": missing,
        "extra": extra,
        "pin_count": len(expected),
        "net_count": len(net_names),
        "command": " ".join(command),
        "stdout": proc.stdout,
        "stderr": proc.stderr,
    }


def _footprint_path(project, lib: str, name: str) -> Path:
    if lib == "PiPod":
        return project.outdir / "lib" / "PiPod.pretty" / f"{name}.kicad_mod"
    return KICAD_FOOTPRINT_DIR / f"{lib}.pretty" / f"{name}.kicad_mod"


def check_footprints(project) -> dict:
    """Compare each part's symbol pins with its footprint pad names."""
    missing_footprints: list[dict] = []
    pins_without_pads: list[dict] = []
    pads_without_pins: list[dict] = []
    checked = 0

    for part in project.iter_parts():
        if not part.fp:
            continue
        lib, name = part.fp.split(":", 1)
        path = _footprint_path(project, lib, name)
        checked += 1
        if not path.is_file():
            missing_footprints.append({
                "ref": part.ref,
                "footprint": part.fp,
                "path": str(path),
            })
            continue
        footprint = parse_sexpr_one(path.read_text(encoding="utf-8"))
        pads: list[tuple[str, str]] = []
        for pad in children(footprint, "pad"):
            pad_name = str(pad[1]) if len(pad) > 1 else ""
            pad_type = str(pad[2]) if len(pad) > 2 else ""
            pads.append((pad_name, pad_type))

        pins = set(part.pin_numbers)
        pad_names = {pad_name for pad_name, _ in pads if pad_name}
        for pin in part.pin_numbers:
            if pin not in pad_names:
                pins_without_pads.append({
                    "ref": part.ref,
                    "footprint": part.fp,
                    "pin": pin,
                })
        for pad_name, pad_type in pads:
            if not pad_name or pad_type == "np_thru_hole":
                continue
            if pad_name not in pins:
                pads_without_pins.append({
                    "ref": part.ref,
                    "footprint": part.fp,
                    "pad": pad_name,
                    "type": pad_type,
                })

    return {
        "ok": not missing_footprints and not pins_without_pads
        and not pads_without_pins,
        "missing_footprints": missing_footprints,
        "pins_without_pads": pins_without_pads,
        "pads_without_pins": pads_without_pins,
        "checked": checked,
    }
