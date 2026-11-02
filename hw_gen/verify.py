#!/usr/bin/env python3
"""Regenerates hardware/, then runs ERC, netlist and footprint checks and writes a JSON summary."""
import os,json,sys
sys.path.insert(0,os.path.dirname(__file__))
import design,check
KC=r"C:\Program Files\KiCad\10.0\bin\kicad-cli.exe"
res={}
res['erc']=check.run_erc(design.OUT)
res['netlist']=check.check_netlist(design.p,KC)
res['footprints']=check.check_footprints(design.p)
for k,v in res.items():
    print(k,json.dumps(v,default=str)[:700])
