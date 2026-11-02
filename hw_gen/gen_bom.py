#!/usr/bin/env python3
"""Builds hardware/PiPod_BOM.csv from kicadgen's per-part export.

The per-part export is kept in hw_gen/_parts.csv so this can be re-run after the
grouped BOM has replaced hardware/PiPod_BOM.csv. Supplier facts are snapshots from
2026-11-01/02 and must be checked again before ordering.
"""
import csv, os, re, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "hardware", "PiPod_BOM.csv")
PARTS = os.path.join(HERE, "_parts.csv")
SNAP = "2026-11-01/02"
RECHECK = "recheck before ordering"
EXPORT = "Reference,Value,Qty,Manufacturer,MPN,Footprint,Symbol,DNP,Sheet,Description,Verification"
COLS = ["Category","References","Qty","Value","Manufacturer","MPN","Supplier","Supplier SKU",
        "Datasheet URL","KiCad Symbol","KiCad Footprint","Package","Ratings","Populated/DNP",
        "Verification Status","Notes"]
ORDER = ["Mandatory","Optional/DNP","Mechanical","External modules and Off-board"]
MECH = {"SW3","SW4","SW5","SW6","SW7","SW8","J3","J7","J9"}

def first_line(path):
    with open(path, encoding="utf8") as f:
        return f.readline().strip()

src_head = first_line(OUT) if os.path.exists(OUT) else ""
if not os.path.exists(PARTS):
    if src_head != EXPORT:
        raise SystemExit(f"{OUT} is not kicadgen's per-part export; run design.py first")
    shutil.copyfile(OUT, PARTS)
elif src_head == EXPORT:
    shutil.copyfile(OUT, PARTS)
if first_line(PARTS) != EXPORT:
    raise SystemExit(f"{PARTS} is not kicadgen's per-part export; run design.py first")

parts = list(csv.DictReader(open(PARTS, encoding="utf8", newline="")))
missing = [c for c in EXPORT.split(",") if not parts or c not in parts[0]]
if missing:
    raise SystemExit(f"{PARTS} is missing columns: {', '.join(missing)}")

# (supplier, Digi-Key SKU, snapshot note); Digi-Key unless noted. Blank SKU = not listed.
SUP = {
 "SC1512-A4":("Digi-Key","","Digi-Key stock 3,515, Mouser 1,039, 22-week lead time"),
 "AOTA-B201610S3R3-101-T":("Arrow or Verical","","no Digi-Key or Mouser offer, sourcing risk"),
 "C0603C475K8PACTU":("Digi-Key","399-C0603C475K8PACTUTR-ND","stock 125,154"),
 "C0603C104K4RACTU":("Digi-Key","","stock 7,248,230"),
 "C0603C105K4RACTU":("Digi-Key","","stock not read"),
 "LMK212BJ226MG-T":("Digi-Key","587-1958-1-ND","stock 525,170, status Not For New Designs"),
 "LMK325BJ107MM-T":("Digi-Key","587-1965-2-ND","stock 92,909"),
 "EMK212ABJ106KD-T":("Digi-Key","","stock 55,760"),
 "GRM1885C2A150JA01D":("Digi-Key","490-1331-2-ND","stock 645,869"),
 "BLM18KG601SN1D":("Digi-Key","490-5258-2-ND","stock 1,152,845"),
 "DFE322512F-2R2M=P2":("Digi-Key","490-DFE322512F-2R2M=P2TR-ND","stock 18,854"),
 "LTST-C171KGKT":("Digi-Key","160-1426-2-ND","stock 400,708"),
 "JS102011SAQN":("Digi-Key","401-1999-2-ND","stock 53,695"),
 "SM02B-SRSS-TB(LF)(SN)":("Digi-Key","455-SM02B-SRSS-TBTR-ND","stock 12,160"),
 "SM04B-SRSS-TB(LF)(SN)":("Digi-Key","","comfortably stocked per snapshot"),
 "S2B-PH-SM4-TB(LF)(SN)":("Digi-Key","","comfortably stocked per snapshot"),
 "RC0603FR-0780K6L":("Digi-Key","","stock 153,921"),
 "SJ2-35954D-SMT-TR":("Digi-Key","","stock 820; the Digi-Key datasheet link says discontinued while the status is Active, flag for confirmation"),
 "PPTC071LFBN-RC":("Digi-Key","S7005-ND","stock not read"),
 "PPTC091LFBN-RC":("Digi-Key","S7007-ND","stock not read"),
 "PPTC131LFBN-RC":("Digi-Key","S7011-ND","stock 1,180"),
 "PREC008DAAN-RC":("Digi-Key","35-PREC008DAAN-RC-ND","stock 2,524"),
 "PREC010SAAN-RC":("Digi-Key","35-PREC010SAAN-RC-ND","stock 1,236"),
 "PREC006SAAN-RC":("Digi-Key","","stock 31,829"),
 "ABM8-272-T3":("Digi-Key","","constrained supply"),
 "TPS25200DRVR":("Digi-Key","","stock only 11, buy early"),
 "TPS2116DRLR":("Digi-Key","","stock 177"),
 "TPS63001DRCR":("Digi-Key","","stock 17,671, Mouser 0"),
 "DRV2605LDGST":("Digi-Key","","stock 2,475, Mouser 2,007"),
 "BQ24074RGTR":("Digi-Key","","status Active"),
 "DM3AT-SF-PEJM5(40)":("Digi-Key","","reel-packed, Mouser 16,013, unsuffixed code sold at Digi-Key, confirm which can be bought in small quantity"),
 "SMF5.0A":("Digi-Key","","stock not read"),
 "USBLC6-2SC6":("Digi-Key","","comfortably stocked per snapshot"),
 "AP2112K-1.8TRG1":("Digi-Key","","comfortably stocked per snapshot"),
 "AO3401A":("Digi-Key","","comfortably stocked per snapshot"),
 "TLV320DAC3100IRHBR":("Digi-Key","","stock not read"),
}

def supply_of(mpn):
    if mpn in SUP:
        return SUP[mpn]
    if re.match(r"RC\d{4}", mpn):
        return ("Digi-Key","","standard catalog part, not individually checked")
    return ("Digi-Key","","stock not read")

TI = {"TLV320DAC3100IRHBR":"tlv320dac3100","BQ24074RGTR":"bq24074","TPS2116DRLR":"tps2116",
      "TPS25200DRVR":"tps25200","TPS63001DRCR":"tps63001","DRV2605LDGST":"drv2605l"}
DS = {
 "SC1512-A4":"https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf",
 "SJ2-35954D-SMT-TR":"https://www.sameskydevices.com/product/resource/sj2-3595x-smt.pdf",
 "USB4105-GF-A":"https://gct.co/files/drawings/usb4105.pdf",
 "USBLC6-2SC6":"https://www.st.com/resource/en/datasheet/usblc6-2.pdf",
}

def datasheet(mpn):
    if mpn in TI:
        return f"https://www.ti.com/lit/ds/symlink/{TI[mpn]}.pdf"
    return DS.get(mpn, "")

RATINGS = {
 "C0603C104K4RACTU":"16 V X7R", "C0603C105K4RACTU":"16 V X7R", "C0603C475K8PACTU":"10 V X5R",
 "EMK212ABJ106KD-T":"16 V X5R", "LMK212BJ226MG-T":"10 V X5R", "LMK325BJ107MM-T":"10 V X5R",
 "GRM1885C2A150JA01D":"100 V C0G", "BLM18KG601SN1D":"600 ohm 1.3 A",
 "DFE322512F-2R2M=P2":"2.2 uH 2.6 A", "SMF5.0A":"5 V standoff",
}

PACKAGE_RULES = [
 (r"^QFN-(\d+)-1EP", "QFN-{0}"), (r"^VQFN-(\d+)-1EP", "VQFN-{0}"),
 (r"^Texas_RHB0032E_VQFN-(\d+)-", "VQFN-{0}"), (r"^WSON-(\d+)-", "WSON-{0}"),
 (r"^SOT-583-(\d+)$", "SOT-583"), (r"^SOT-23-(\d+)$", "SOT-23-{0}"), (r"^SOT-23$", "SOT-23"),
 (r"^Texas_DRC0010J$", "VSON-10"), (r"^C_(\d{4})_\d{4}Metric$", "{0}"),
 (r"^R_(\d{4})_\d{4}Metric$", "{0}"), (r"^LED_(\d{4})_\d{4}Metric$", "{0}"),
 (r"^L_(\d{4})_\d{4}Metric$", "{0}"), (r"^L_Abracon_AOTA-B(\d{6})S$", "2016"),
 (r"^Crystal_SMD_(\d{4})-\d+Pin_[\d.]+x[\d.]+mm$", "{0}"), (r"^TSSOP-(\d+)_", "TSSOP-{0}"),
 (r"^SW_SPST_PTS647", "PTS647"), (r"^SW_SPDT_CK_JS102011SAQN$", "SMD slide switch"),
 (r"^D_SMF$", "SMF"), (r"^USB_C_Receptacle", "USB-C"), (r"^microSD_", "microSD"),
 (r"^JST_PH_S2B-PH-SM4-TB_", "JST PH 2-pin"), (r"^JST_SH_SM02B-SRSS-TB_", "JST SH 2-pin"),
 (r"^JST_SH_SM04B-SRSS-TB_", "JST SH 4-pin"),
 (r"^PinSocket_(1x\d+)_P([\d.]+)mm_", "{0} {1} mm socket"),
 (r"^PinHeader_(1x\d+|2x\d+)_P([\d.]+)mm_", "{0} {1} mm header"),
 (r"^TestPoint_Pad_D([\d.]+)mm$", "{0} mm pad"), (r"^Jack_3\.5mm", "3.5 mm jack"),
 (r"^WheelElectrode_Placeholder$", "PCB electrode"),
]

def package_of(fp):
    base = fp.split(":")[-1]
    for pat, fmt in PACKAGE_RULES:
        m = re.match(pat, base)
        if m:
            return fmt.format(*m.groups())
    return ""

def refkey(ref):
    m = re.match(r"([A-Za-z]+)(\d+)$", ref)
    return (m.group(1), int(m.group(2))) if m else (ref, 0)

def is_nopart(refs):
    return bool(refs) and all(re.match(r"^(TP|E)\d+$", r) for r in refs)

def merge_verification(vals):
    u = list(dict.fromkeys(v.strip() for v in vals if v.strip()))
    return "; ".join(v for v in u if not any(v != w and v in w for w in u))

def group_key(r):
    cat = "Mechanical" if r["Reference"] in MECH else "Optional/DNP" if r["DNP"].strip().lower() == "yes" else "Mandatory"
    mpn = r["MPN"].strip()
    return (cat, mpn, "" if mpn else r["Value"].strip(), "" if mpn else r["Footprint"].strip(), r["DNP"].strip().lower() == "yes")

groups = {}
for r in parts:
    groups.setdefault(group_key(r), []).append(r)

rows = []
for (cat, mpn, _v, _f, dnp), grp in groups.items():
    refs = sorted((r["Reference"] for r in grp), key=refkey)
    value = " / ".join(dict.fromkeys(r["Value"].strip() for r in grp))
    mfr = " / ".join(dict.fromkeys(r["Manufacturer"].strip() for r in grp if r["Manufacturer"].strip()))
    if is_nopart(refs):
        supplier, sku, notes = "", "", "No part"
    elif mpn:
        supplier, sku, note = supply_of(mpn)
        notes = f"{SNAP} snapshot: {note}; {RECHECK}"
    else:
        supplier, sku, notes = "", "", f"no MPN in the export ({SNAP} snapshot); {RECHECK}"
    rows.append([cat, ", ".join(refs), len(refs), value, mfr, mpn, supplier, sku,
                 datasheet(mpn), " / ".join(dict.fromkeys(r["Symbol"].strip() for r in grp)),
                 grp[0]["Footprint"].strip(), package_of(grp[0]["Footprint"].strip()),
                 RATINGS.get(mpn, ""), "DNP" if dnp else "Populated",
                 merge_verification([r["Verification"] for r in grp]), notes])

def fixed_row(value, mfr, mpn, supplier, package, notes):
    return ["External modules and Off-board", "", 1, value, mfr, mpn, supplier, "", "", "", "",
            package, "", "Off-board", "provisional", notes]

fixed = [
 fixed_row("MPR121 capacitive touch module", "Adafruit", "1982", "Adafruit", "breakout module",
           "I2C address 0x5B set by R90; plugs into J7 and J9; not on the board; " + RECHECK),
 fixed_row("DAC test board", "Adafruit", "6309", "Adafruit", "breakout board",
           "audio path test board; not on the board; " + RECHECK),
 fixed_row("WM8960 audio board", "Waveshare", "", "Waveshare", "module",
           "backup codec module; header mapping from the module schematic; not on the board; " + RECHECK),
 fixed_row("2.2 in ILI9341 SPI module", "", "", "", "module",
           "pin order unconfirmed; not on the board; " + RECHECK),
 fixed_row("1000 mAh protected LiPo, 2-pin JST-PH", "", "", "", "JST-PH 2-pin",
           "capacity and connector assumed; polarity to confirm; " + RECHECK),
 fixed_row("ERM coin motor, 3.3 V", "", "", "", "",
           "not selected; must be rated for 3.3 V; " + RECHECK),
 fixed_row("Qwiic cable, 4-pin JST-SH", "", "", "", "JST-SH 4-pin",
           "4-pin JST-SH cable; " + RECHECK),
]
rows += fixed
rows.sort(key=lambda r: (ORDER.index(r[0]), refkey(r[1].split(", ")[0] if r[1] else "")))

with open(OUT, "w", encoding="utf8", newline="") as f:
    w = csv.writer(f, lineterminator="\n")
    w.writerow(COLS)
    w.writerows(rows)

qty = sum(r[2] for r in rows)
out_refs = [x for r in rows for x in (r[1].split(", ") if r[1] else [])]
assert qty == len(parts) + len(fixed), (qty, len(parts), len(fixed))
assert sorted(out_refs) == sorted(r["Reference"] for r in parts), "reference set differs from the export"
assert len(set(out_refs)) == len(out_refs), "a reference appears more than once"
print(f"rows {len(rows)}")
print(f"qty {qty} = {len(parts)} exported parts + {len(fixed)} fixed rows")
print(f"references {len(out_refs)} of {len(parts)}, each exactly once")
loose = [r for r in rows if not r[5] and not is_nopart(r[1].split(", ") if r[1] else [])]
for r in loose:
    print("no MPN:", r[1] or "(fixed row)", "|", r[3])
print(f"empty MPN outside TP/E: {len(loose)}")
