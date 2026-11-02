#!/usr/bin/env python3
"""Acceptance self-test for :mod:`kicadgen`.

Generates a two-sheet hierarchy into ``hw_gen/_selftest``, then verifies:

* kicad-cli ERC loads the design and reports no ``lib_symbol_issues``,
* the exported netlist matches every declared pin-to-net connection,
* kicad-cli can export a PDF,
* re-running ``write()`` produces byte-identical source files,
* footprint pads line up with symbol pins.

Run with::

    python hw_gen/selftest.py
"""

from __future__ import annotations

import hashlib
import math
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import kicadgen  # noqa: E402
from check import check_footprints, check_netlist, run_erc  # noqa: E402

OUTDIR = HERE / "_selftest"
KICAD_CLI = str(kicadgen.KICAD_CLI)
PROJECT_NAME = "PiPodGenSelftest"


def _mcu_nets(project: kicadgen.Project):
    """Assign every RP2354B pin: GPIOs get nets, everything else NC."""
    nets: dict[str, str] = {}
    nc: list[str] = []
    for number, name, _etype in project.symbol_pins("MCU_RaspberryPi:RP2354B"):
        if name == "GND":
            nets[number] = "GND"
        elif name in ("IOVDD", "ADC_AVDD", "QSPI_IOVDD", "USB_OTP_VDD",
                      "VREG_VIN"):
            nets[number] = "+3V3"
        elif name in ("DVDD", "VREG_AVDD"):
            nets[number] = "+1V1"
        elif name == "VREG_PGND":
            nets[number] = "GND"
        elif name == "USB_DP":
            nets[number] = "USB_DP"
        elif name == "USB_DM":
            nets[number] = "USB_DM"
        elif name.startswith("GPIO"):
            nets[number] = name.split("/")[0]
        else:
            nc.append(number)
    return nets, nc


def build_project(outdir: Path) -> kicadgen.Project:
    p = kicadgen.Project(
        PROJECT_NAME,
        outdir,
        "PiPod generator self-test",
        "A",
        company="BanyanCode",
        date="2026-11-01",
    )

    p.custom_symbol(
        "TestPoint2",
        [("1", "TP", "passive", "R"), ("2", "RETURN", "passive", "L")],
        footprint="",
        description="Self-test two-pad test point",
        reference="TP",
    )

    mcu = p.sheet("MCU", "mcu.kicad_sch", page_size="A3")
    mcu_nets, mcu_nc = _mcu_nets(p)
    mcu.part(
        "U1",
        "MCU_RaspberryPi:RP2354B",
        "RP2354B",
        "Package_DFN_QFN:QFN-80-1EP_10x10mm_P0.4mm_EP3.4x3.4mm",
        mcu_nets,
        mpn="RP2354B",
        mfr="Raspberry Pi",
        nc=mcu_nc,
        desc="Dual-architecture MCU (Cortex-M33 / Hazard3)",
        fields={"Voltage": "3.3 V"},
        group="mcu",
    )
    mcu.part(
        "R1",
        "Device:R",
        "10k",
        "Resistor_SMD:R_0402_1005Metric",
        {"1": "+3V3", "2": "GPIO0"},
        mpn="RC0402FR-0710KL",
        mfr="Yageo",
        fields={"Tolerance": "1%", "Power": "0.063 W"},
        group="mcu",
    )
    mcu.part(
        "C1",
        "Device:C",
        "100nF",
        "Capacitor_SMD:C_0402_1005Metric",
        {"1": "+3V3", "2": "GND"},
        mpn="GRM155R71C104KA88D",
        mfr="Murata",
        dnp=True,
        desc="Decoupling capacitor",
        group="mcu",
    )
    mcu.note("MCU sheet\nRP2354B, USB 2.0 device port\n+3V3 / +1V1 rails")
    mcu.pwr_flag("+3V3")

    power = p.sheet("Power", "power.kicad_sch", page_size="A3")
    power.part(
        "U2",
        "Regulator_Linear:AP2112K-1.8",
        "AP2112K-1.8",
        "Package_TO_SOT_SMD:SOT-23-5",
        {"1": "VBUS", "2": "GND", "3": "VBUS", "5": "+1V1"},
        mpn="AP2112K-1.8TRG1",
        mfr="Diodes Incorporated",
        nc=("4",),
        desc="1.8 V LDO regulator",
        group="power",
    )
    power.part(
        "U3",
        "Power_Protection:USBLC6-2SC6",
        "USBLC6-2SC6",
        "Package_TO_SOT_SMD:SOT-23-6",
        {"1": "USB_DP", "2": "GND", "3": "USB_DM", "4": "USB_DM",
         "5": "VBUS", "6": "USB_DP"},
        mpn="USBLC6-2SC6",
        mfr="STMicroelectronics",
        desc="USB 2.0 ESD protection",
        group="power",
    )
    power.part(
        "J1",
        "Connector:USB_C_Receptacle_USB2.0_16P",
        "USB_C_Receptacle_USB2.0_16P",
        "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
        {
            "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH": "GND",
            "A4": "VBUS", "A9": "VBUS", "B4": "VBUS", "B9": "VBUS",
            "A6": "USB_DP", "B6": "USB_DP", "A7": "USB_DM", "B7": "USB_DM",
            "A5": "CC1", "B5": "CC2",
        },
        mpn="TYPE-C-31-M-12",
        mfr="HRO",
        nc=("A8", "B8"),
        desc="USB 2.0 Type-C receptacle",
        group="power",
    )
    power.part(
        "TP1",
        "PiPod:TestPoint2",
        "TP",
        "",
        {"1": "+1V1", "2": "GND"},
        mpn="",
        mfr="",
        desc="Generator self-test custom symbol",
        group="test",
    )
    power.note("Power sheet\nUSB-C -> ESD -> 1.8 V LDO\nGND / VBUS / USB data")
    power.pwr_flag("GND")
    power.pwr_flag("VBUS")
    return p


def snapshot(outdir: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for path in sorted(outdir.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix in (".kicad_sch", ".kicad_pro", ".kicad_sym") or \
                path.name in ("sym-lib-table", "fp-lib-table"):
            rel = path.relative_to(outdir).as_posix()
            result[rel] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def _intersects(a, b) -> bool:
    return not (
        a[2] <= b[0] + 1e-6 or b[2] <= a[0] + 1e-6
        or a[3] <= b[1] + 1e-6 or b[3] <= a[1] + 1e-6
    )


def layout_problems(project) -> list[str]:
    """Verify packing has no overlaps and every stub is on the 1.27 mm grid."""
    problems: list[str] = []
    checked_parts = 0
    for sheet in project.sheets:
        pack, layouts = project._sheet_pack(sheet)
        boxes = []
        for part in sheet.parts:
            checked_parts += 1
            ox, oy = pack.placements[part.ref].origin
            box = layouts[part.ref].box
            boxes.append((part.ref, (ox + box.xmin, oy + box.ymin,
                                     ox + box.xmax, oy + box.ymax)))
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                if _intersects(boxes[i][1], boxes[j][1]):
                    problems.append(
                        f"{sheet.name}: {boxes[i][0]} overlaps {boxes[j][0]}")
        for part in sheet.parts:
            layout = layouts[part.ref]
            wire_ends: set[tuple[float, float]] = set()
            for x1, y1, x2, y2 in layout.wires:
                length = math.hypot(x2 - x1, y2 - y1)
                if abs(length - kicadgen.STUB) > 1e-6:
                    problems.append(
                        f"{sheet.name}/{part.ref}: stub length {length:.3f}")
                for value in (x1, y1, x2, y2):
                    if abs(value / kicadgen.GRID - round(value / kicadgen.GRID)) > 1e-6:
                        problems.append(
                            f"{sheet.name}/{part.ref}: off-grid point {value}")
                wire_ends.add((round(x1, 4), round(y1, 4)))
                wire_ends.add((round(x2, 4), round(y2, 4)))
            for x, y, _rot, net in layout.labels:
                if (round(x, 4), round(y, 4)) not in wire_ends:
                    problems.append(
                        f"{sheet.name}/{part.ref}: label {net} not on a wire end")
    print(f"layout: {checked_parts} parts, {len(problems)} problems")
    return problems


def main() -> int:
    if OUTDIR.exists():
        shutil.rmtree(OUTDIR)
    OUTDIR.mkdir(parents=True)
    print(f"output directory: {OUTDIR}")

    project = build_project(OUTDIR)
    project.write()

    expected_files = [
        f"{PROJECT_NAME}.kicad_pro",
        f"{PROJECT_NAME}.kicad_sch",
        "mcu.kicad_sch",
        "power.kicad_sch",
        "lib/PiPod.kicad_sym",
        "sym-lib-table",
        "fp-lib-table",
    ]
    for name in expected_files:
        path = OUTDIR / name
        assert path.is_file(), f"missing generated file: {path}"
        print(f"  wrote {path.relative_to(OUTDIR).as_posix()} "
              f"({path.stat().st_size} bytes)")

    assert not list(OUTDIR.rglob("*.lck")), "lock file was written"
    assert not (OUTDIR / ".history").exists(), ".history was written"

    first = snapshot(OUTDIR)
    shutil.rmtree(OUTDIR)
    project = build_project(OUTDIR)
    project.write()
    second = snapshot(OUTDIR)
    assert first == second, "regeneration was not byte-identical"
    print(f"determinism: {len(first)} files byte-identical after a full "
          f"rebuild with a fresh Project instance")

    print("\n[layout]")
    problems = layout_problems(project)
    for problem in problems[:20]:
        print(f"  {problem}")
    assert not problems, "layout overlaps or off-grid stubs found"

    version = subprocess.run([KICAD_CLI, "--version"], capture_output=True,
                             text=True)
    print(f"kicad-cli --version -> {version.stdout.strip()} "
          f"(exit {version.returncode})")

    print("\n[ERC]")
    erc = run_erc(OUTDIR, KICAD_CLI)
    counts = {kind: len(items) for kind, items in sorted(erc.items())}
    total = sum(counts.values())
    print(f'  command: "{KICAD_CLI}" sch erc --format json --severity-all '
          f'"{OUTDIR / (PROJECT_NAME + ".kicad_sch")}"')
    print(f"  Found {total} violations")
    for kind, count in counts.items():
        print(f"    {kind}: {count}")
    assert "lib_symbol_issues" not in erc, "ERC reported lib_symbol_issues"
    parse_hits = [
        item for items in erc.values() for item in items
        if "parse" in str(item.get("description", "")).lower()
        or "load" in str(item.get("description", "")).lower()
    ]
    assert not parse_hits, f"ERC reported parse/load errors: {parse_hits[:3]}"
    print("  no lib_symbol_issues, no parse errors")

    print("\n[netlist]")
    net_result = check_netlist(project, KICAD_CLI)
    print(f"  command: {net_result['command']}")
    if net_result["stdout"].strip():
        print(f"  stdout: {net_result['stdout'].strip()}")
    if net_result["stderr"].strip():
        print(f"  stderr: {net_result['stderr'].strip()}")
    print(f"  pins checked: {net_result['pin_count']}, "
          f"nets: {net_result['net_count']}")
    if not net_result["ok"]:
        for item in net_result["missing"][:20]:
            print(f"  MISSING {item}")
        for item in net_result["extra"][:20]:
            print(f"  EXTRA   {item}")
    assert net_result["ok"], "netlist mismatches found"
    print("  zero mismatches")

    print("\n[footprints]")
    fp_result = check_footprints(project)
    print(f"  footprints checked: {fp_result['checked']}")
    for item in fp_result["missing_footprints"]:
        print(f"  MISSING FOOTPRINT {item}")
    for item in fp_result["pins_without_pads"]:
        print(f"  PIN WITHOUT PAD {item}")
    for item in fp_result["pads_without_pins"]:
        print(f"  PAD WITHOUT PIN {item}")
    assert fp_result["ok"], "footprint/pin mismatches found"
    print("  all symbol pins map to footprint pads")

    print("\n[PDF]")
    pdf_path = OUTDIR / "selftest.pdf"
    pdf_cmd = [KICAD_CLI, "sch", "export", "pdf", "--output", str(pdf_path),
               str(project.root_schematic_path)]
    pdf = subprocess.run(pdf_cmd, capture_output=True, text=True)
    print(f"  command: {' '.join(pdf_cmd)}")
    print(f"  exit: {pdf.returncode}")
    if pdf.stdout.strip():
        print(f"  stdout: {pdf.stdout.strip()}")
    if pdf.stderr.strip():
        print(f"  stderr: {pdf.stderr.strip()}")
    assert pdf.returncode == 0, "PDF export failed"
    assert pdf_path.is_file() and pdf_path.stat().st_size > 0, "empty PDF"
    print(f"  wrote {pdf_path.name} ({pdf_path.stat().st_size} bytes)")

    print("\n[BOM]")
    bom_path = OUTDIR / "selftest-bom.csv"
    project.export_bom(bom_path)
    print(f"  wrote {bom_path.name} ({bom_path.stat().st_size} bytes)")
    print("  " + bom_path.read_text(encoding="utf-8").splitlines()[0])
    assert bom_path.read_text(encoding="utf-8").count("\n") == 8

    print("\nSELFTEST PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
