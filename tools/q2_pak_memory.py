#!/usr/bin/env python3
"""Quake II RP2350 offline memory estimator (requires q2_pak_level_groups.py).

Reads real PAK/BSP/WAL contents, existing map-assets.csv and optional flash
selection to estimate the resident 32-bit collision/render structures of maps.
This is a STATIC SCENARIO, not a runtime peak-usage measurement.

Usage (from frank-quake2 repo root):
 python tools/q2_pak_memory.py --paks C:/Q2/baseq2/pak0.pak C:/Q2/baseq2/pak1.pak C:/Q2/baseq2/pak2.pak --analysis q2-analysis --selection q2-offline-assessment/flash-selection.csv --elf-map build/frank-quake2.elf.map --out q2-memory
"""
from __future__ import annotations
import argparse
import csv
from pathlib import Path
import re
import struct
from collections import defaultdict

from q2_pak_level_groups import PakSet, parse_bsp, align

MIB = 1 << 20
KIB = 1 << 10
PSRAM_BASE = 0x11000000
# RP2350 / Thumb 32-bit structs, verified against game/q_shared.h,
# ref_soft/r_model.h, qcommon/cmodel.c in frank-quake2, Oct 2026.
DISK_SIZE = {1:20, 2:12, 3:0, 4:28, 5:76, 6:20, 7:1, 8:28,
             9:2, 10:2, 11:4, 12:4, 13:48, 14:12, 15:4}
# Lumps index per qfiles.h: 0 entities, 1 planes, 2 verts, 3 VIS,
# 4 nodes, 5 texinfo, 6 faces, 7 lighting, 8 leaves, 9 leaffaces,
# 10 leafbrushes, 11 edges, 12 surfedges, 13 models, 14 brushes, 15 sides.
# Renderer: mvertex=12, medge=8, mtexinfo=52, msurface=68,
# mleaf=36, mnode=40, dmodel=48, pointer=4, mplane=20.
# CM: mapsurface=24, cleaf=12, cbrush=16, cbrushside=4,
# cmodel=40, cnode=12, cplane=20.
WAL_FORCE_MIP0 = 0x04 | 0x08 | 0x10 | 0x20 | 0x40  # SKY/WARP/TRANS33/TRANS66/FLOWING


def csv_read(path):
    with Path(path).open('r', newline='', encoding='utf-8-sig') as f:
        return list(csv.DictReader(f))


def csv_write(path, rows, cols):
    with Path(path).open('w', newline='', encoding='utf-8-sig') as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
        w.writeheader(); w.writerows(rows)


def limb(lumps, i):
    size = lumps[i][1]
    unit = DISK_SIZE[i]
    if unit == 0 or size % unit:
        raise ValueError(f'BSP lump {i}: {size} bytes not a multiple of {unit}')
    return size // unit


def bsp_allocations(lumps):
    # Matches the exact eight CM_ALIGN expressions in cmodel.c:CM_LoadMap.
    nt, nl, nlb = limb(lumps, 5), limb(lumps, 8), limb(lumps, 10)
    np, nb, nbs = limb(lumps, 1), limb(lumps, 14), limb(lumps, 15)
    nm, nn = limb(lumps, 13), limb(lumps, 4)
    nv, ne, ns = limb(lumps, 2), limb(lumps, 11), limb(lumps, 12)
    nf, nlf = limb(lumps, 6), limb(lumps, 9)
    cm = [nt*24, (nl+1)*12, (nlb+1)*2, (np+12)*20,
          (nb+1)*16, (nbs+6)*4, (nm+1)*40, (nn+6)*12,
          lumps[3][1]+4]
    cm_bytes = sum(align(x) for x in cm)
    # r_model.c:Mod_LoadBrushModel uses CM_SharedPlanes and CM_SharedVis;
    # hence neither has a second copy in the renderer's Hunk.
    render = [(nv+8)*12, (ne+13)*8, (ns+24)*4,
              lumps[7][1]//3, (nt+6)*52, (nf+6)*68,
              nlf*4, nl*36, nn*40, nm*48]
    render_bytes = sum(align(x) for x in render)
    # Only the lumps passed through CM_LumpBegin/Mod_LumpBegin are copied
    # wholesale. LIGHTING is decoded in small chunks, and VIS is shared.
    # These temporary blocks are freed after each lump, so NOT resident.
    loader_lumps = {5, 8, 10, 1, 14, 15, 13, 4, 17, 18,
                    2, 11, 12, 6, 9}
    transient = max((lumps[i][1] + 1 for i in loader_lumps), default=0)
    return dict(collision_bytes=cm_bytes, renderer_bytes=render_bytes,
                bsp_resident_bytes=cm_bytes+render_bytes,
                transient_one_lump_bytes=transient,
                bsp_lighting_decoded_bytes=lumps[7][1]//3,
                bsp_vis_shared_bytes=lumps[3][1],
                bsp_faces=nf,bsp_texinfo=nt,bsp_models=nm)


def wal_memory(paks, name, full_mip0):
    h = paks.read(name, 100)
    if h is None or len(h) < 100:
        return None
    w, hgt = struct.unpack_from('<II', h, 32)
    if not (0 < w <= 8192 and 0 < hgt <= 8192):
        return None
    # r_image.c:R_LoadWalPixels: WH * (340 or 84) / 256.
    # The 32-bit arena allocator adds a block header/alignment separately.
    pixels = w * hgt
    return (pixels * 340 // 256) if full_mip0 else (pixels * 84 // 256)


def wal_for_map(paks, raw, lumps, mipcap):
    """Texinfo traverses ALL entries, so return unique texture names and
    their most expensive requested mip. A mixed-use WAL may be reloaded.
    """
    off, size = lumps[5]
    usage = {}
    for idx in range(size // 76):
        pos = off + idx * 76
        flag = struct.unpack_from('<i', raw, pos + 32)[0]
        name = raw[pos+40:pos+72].split(b'\0',1)[0].decode('latin-1').lower()
        if not name:
            continue
        key='textures/'+name.replace('\\','/')+'.wal'
        mip0 = mipcap < 1 or bool(flag & WAL_FORCE_MIP0)
        usage[key] = usage.get(key, False) or mip0
    rows=[]
    resident=0; missing=0
    for path, mip0 in sorted(usage.items()):
        n = wal_memory(paks, path, mip0)
        if n is None: missing+=1
        else:
            # q_arena.c: ARENA_HDR = sizeof(void*) * 2 + sizeof(int) * 2 = 16
            # on the 32-bit RP2350; each live WAL has its own arena block.
            resident += align(n) + 16
        rows.append(dict(name=path, mip='0' if mip0 else '1',
                         found=int(n is not None), memory_bytes=n if n is not None else '',
                         allocated_bytes=align(n)+16 if n is not None else '',
                         pak_bytes=paks.files[path][2] if path in paks.files else ''))
    return rows, resident, missing


def psram_reserved(elf_map_path):
    if not elf_map_path:
        return None
    s=Path(elf_map_path).read_text(encoding='latin-1')
    heap = re.search(r'^\s+0x([0-9a-fA-F]+)\s+__psram_heap_start__\s*=',s,re.M)
    if heap:
        x=int(heap.group(1),16)
        if PSRAM_BASE <= x < PSRAM_BASE+16*MIB:
            return x - PSRAM_BASE
    bss=re.search(r'^\.psram_bss\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)',s,re.M)
    if bss:
        return align(int(bss.group(1),16),4096)
    return None


def linker_objects(path):
    '''Read PSRAM contributions from the GNU ld .map; two-line section
    records (e.g. .psram_bss.prof) need special handling.'''
    if not path:
        return []
    s=Path(path).read_text(encoding='latin-1')
    m=re.search(r'^\.psram_bss\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+',s,re.M)
    if not m:
        return []
    body=s[m.end():].split('\n.psram_heap',1)[0]
    out=[]
    pending=None
    for line in body.splitlines():
        section=re.match(r'^\s+\.(ext_ram\.bss|psram_bss(?:\.\w+)?)\s*$',line)
        if section:
            pending=section.group(1)
            continue
        one=re.match(r'^\s+\.(ext_ram\.bss|psram_bss(?:\.\w+)?)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+\.obj)',line)
        if one:
            out.append(dict(section=one.group(1),address='0x'+one.group(2),size_bytes=int(one.group(3),16),object=one.group(4)))
            pending=None
            continue
        two=re.match(r'^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+\.obj)',line)
        if two and pending:
            out.append(dict(section=pending,address='0x'+two.group(1),size_bytes=int(two.group(2),16),object=two.group(3)))
            pending=None
    return out


def main():
    ap=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--paks', nargs='+',required=True)
    ap.add_argument('--analysis',type=Path,required=True,help='directory produced by q2_pak_level_groups.py')
    ap.add_argument('--selection',type=Path,help='flash-selection.csv; compare optimized scenario')
    ap.add_argument('--selection-column',default='minimax_common_flash')
    ap.add_argument('--elf-map',type=Path,help='frank-quake2.elf.map for .psram_bss and heap alignment')
    ap.add_argument('--static-reserved',type=lambda v:int(v,0),default=None,
                    help='override static PSRAM reserved bytes (for board without .elf.map)')
    ap.add_argument('--mipcap',type=int,default=1)
    ap.add_argument('--sound-kib',type=int,default=1536,help='upper sound cache budget, default 1536 KiB')
    ap.add_argument('--surfcache-kib',type=int,default=256)
    ap.add_argument('--arena-reserve-kib',type=int,default=192,
                    help='QUAKE_HEAP_RESERVE outside engine arena; not wholly unavailable to malloc')
    ap.add_argument('--zbuffer-in-psram',action='store_true',help='if internal SRAM z-buffer allocation fails')
    ap.add_argument('--out',type=Path,default=Path('q2-memory'))
    args=ap.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    map_rows=csv_read(args.analysis/'map-assets.csv')
    placement=defaultdict(dict)
    for row in map_rows: placement[row['map']][row['name'].lower()]=row
    sel={}
    if args.selection:
        for row in csv_read(args.selection):
            if args.selection_column not in row:
                ap.error(f'No column {args.selection_column} in {args.selection}')
            sel[row['name'].lower()]=row[args.selection_column].strip()=='1'
    static_reserved=args.static_reserved if args.static_reserved is not None else psram_reserved(args.elf_map)
    objects=linker_objects(args.elf_map)
    if objects:
        csv_write(args.out/'linker-psram-objects.csv',
                  sorted(objects,key=lambda r:-r['size_bytes']),
                  ['section','address','size_bytes','object'])
    sound_bytes=args.sound_kib*KIB; surface_bytes=args.surfcache_kib*KIB
    arena_reserve=args.arena_reserve_kib*KIB; zbuffer=320*240*2 if args.zbuffer_in_psram else 0
    rows=[]; wall_rows=[]; errors=[]
    paks=PakSet(args.paks)
    try:
        maps=sorted(n for n in paks.files if n.startswith('maps/') and n.endswith('.bsp'))
        for path in maps:
            map_name=path[5:-4]
            raw=paks.read(path)
            try:
                entities,_,lumps=parse_bsp(raw)
                m=bsp_allocations(lumps)
                wals, wal_bytes, missing=wal_for_map(paks,raw,lumps,args.mipcap)
            except (ValueError,IndexError,struct.error) as e:
                errors.append(f'{map_name}: {e}'); continue
            for w in wals:
                wall_rows.append({'map':map_name,**w})
            p=placement.get(map_name,{})
            current=sum(int(v.get('converted_bytes') or 0) for v in p.values()
                        if v.get('in_current_flash')!='1')
            selected=(sum(int(v.get('converted_bytes') or 0) for name,v in p.items()
                          if not sel.get(name,False)) if sel else None)
            # Known model/skin sets from previous static analysis can include
            # false positives from source-text parsing. Not a strict bound.
            known_unknown=sum(not bool(v.get('converted_bytes')) for v in p.values())
            base=m['bsp_resident_bytes']+wal_bytes+surface_bytes+zbuffer
            row=dict(map=map_name,**m,wal_count=len(wals),wal_missing=missing,
                     wal_resident_bytes=wal_bytes,model_skin_current_bytes=current,
                     model_skin_selected_bytes=selected if selected is not None else '',
                     fixed_surface_cache_bytes=surface_bytes,zbuffer_psram_bytes=zbuffer,
                     runtime_known_current_bytes=base+current,
                     runtime_known_selected_bytes=(base+selected if selected is not None else ''),
                     temp_single_lump_bytes=m['transient_one_lump_bytes'],
                     model_skin_unknown_count=known_unknown)
            if static_reserved is not None:
                row['static_psram_bytes']=static_reserved
                row['left_current_no_sound_bytes']=8*MIB-static_reserved-base-current
                row['left_current_soundcap_bytes']=8*MIB-static_reserved-base-current-sound_bytes
                if selected is not None:
                    row['left_selected_no_sound_bytes']=8*MIB-static_reserved-base-selected
                    row['left_selected_soundcap_bytes']=8*MIB-static_reserved-base-selected-sound_bytes
                else:
                    row['left_selected_no_sound_bytes']=''
                    row['left_selected_soundcap_bytes']=''
                row['arena_limit_approx_bytes']=8*MIB-static_reserved-arena_reserve
                row['left_current_arena_headroom_bytes']=row['left_current_no_sound_bytes']-arena_reserve
                row['left_selected_arena_headroom_bytes']=(row['left_selected_no_sound_bytes']-arena_reserve) if selected is not None else ''
            else:
                row.update(static_psram_bytes='',left_current_no_sound_bytes='',
                    left_current_soundcap_bytes='',left_selected_no_sound_bytes='',
                    left_selected_soundcap_bytes='',arena_limit_approx_bytes='',
                    left_current_arena_headroom_bytes='',left_selected_arena_headroom_bytes='')
            rows.append(row)
    finally:
        paks.close()
    fields=list(rows[0]) if rows else []
    if rows:
        csv_write(args.out/'map-memory.csv',sorted(rows,key=lambda v:-v['runtime_known_current_bytes']),fields)
        csv_write(args.out/'wal-memory.csv',wall_rows,['map','name','mip','found','memory_bytes','allocated_bytes','pak_bytes'])
    report=[]
    report.append('Quake II RP2350 offline memory estimate from actual C allocation formulas')
    report.append('Sources: cmodel.c:CM_LoadMap, r_model.c:Mod_LoadBrushModel, r_image.c:R_LoadWalPixels,')
    report.append('r_surf.c:R_InitCaches, snd_dma.c:S_Init, snd_mem.c:S_CacheBudget, q_arena.c.')
    report.append(f'Maps analyzed: {len(rows)}; Flash selection: {args.selection_column if sel else "current only"}')
    report.append(f'Static PSRAM reservation (including alignment): {static_reserved if static_reserved is not None else "unknown: supply --elf-map"} bytes')
    if objects:
        report.append('Largest static PSRAM modules in this ELF:')
        for o in sorted(objects,key=lambda a:-a['size_bytes'])[:8]:
            report.append(f'  {o["size_bytes"]:9,d} B {o["object"]}')
    if args.elf_map:
        txt=args.elf_map.read_text(encoding='latin-1')
        old=re.search(r'^\s+0x([0-9a-fA-F]+)\s+__q2_assets_offset\s*=', txt, re.M)
        if old:
            offset=int(old.group(1),16)
            report.append(f'Linker FLASH asset offset in this map: 0x{offset:x} ({offset // 1024} KiB)')
            if offset != 0xe1000:
                report.append('WARNING: linker .map was built before the 900-KiB flash-boundary change;')
                report.append('         dynamic PSRAM/size may also change in a later rebuild.')
    report.append(f'Fixed surface cache: {surface_bytes} B; sound cache: 0..{sound_bytes} B; z-buffer PSRAM: {zbuffer} B')
    report.append(f'Engine arena requests leave {arena_reserve} B of the PSRAM heap for other mallocs.')
    report.append('NOTE: arena reserve is headroom, NOT automatically an additional occupied block.')
    report.append('Resident BSP = CM static collision block + render Hunk, with shared planes and VIS counted once.')
    report.append('WAL resident = requested mip data + 16 bytes of allocator header and alignment per unique texture.')
    report.append('')
    for mode, key in [('CURRENT','runtime_known_current_bytes'),('SELECTED','runtime_known_selected_bytes')]:
        if mode=='SELECTED' and not sel:continue
        high=sorted(rows,key=lambda x:-int(x[key]))[:12]
        report.append(f'{mode}: top maps by static inventory + BSP geometry + WAL + surface cache:')
        for a in high:
            other=a['model_skin_selected_bytes'] if mode=='SELECTED' else a['model_skin_current_bytes']
            v=int(a[key]);left=8*MIB-(static_reserved or 0)-v
            report.append(f'  {a["map"]:16s} {v:10,d} B + static; BSP {a["bsp_resident_bytes"]:9,d}, WAL {a["wal_resident_bytes"]:9,d}, MD2/PCX {other:9,d}, left-no-sound {left if static_reserved is not None else "?"} B')
        report.append('')
    if errors:report.extend(['Invalid maps:']+errors)
    report.extend(['', 'LIMITATIONS / INTERPRETATION:',
        '* Model and skin sets come from the earlier static scanner: it can both overcount and undercount.',
        '* Six global C-text matches absent from the PAK are unresolved, not six allocated 0-byte files.',
        '* BSP allocations are computed from exact 32-bit struct sizes and source formulas; runtime headers may differ.',
        '* WAL: worst resident request for each unique map texture; many WALs can later be purged; no resident sound set inferred.',
        '* Dynamic sky PCX (up to six 128x128 faces), sprites, UI graphics, player skins, zone allocs,',
        '  savegame/game state, allocator fragmentation, dynamic spawns and per-load peak buffers are excluded.',
        '* Sound cache 0..configured budget is reclaimable; filled-cache numbers are a stress scenario, not constant occupancy.',
        '* Transient BSP lump buffer is separately listed; do NOT add every BSP source file byte to resident sums.',
        '* Server collision/cmodel allocations for the entity string can add temporary PSRAM; this is not counted.',
        '* Model/skin data actually loaded into PSRAM may include temporary duplication and alignment overhead.',
        '* Crossing 8 MiB in a scenario flags memory pressure, NOT an unconditional proof that a level fails.',
        '* For an accurate guarantee, instrument allocator peaks and fragmentation and correct static model dependencies.'])
    (args.out/'memory-report.txt').write_text('\n'.join(report)+'\n',encoding='utf-8')
    print(f'Analyzed {len(rows)} maps; wrote {args.out/"map-memory.csv"}, wal-memory.csv, memory-report.txt')
    if static_reserved is None:print('WARN: supply --elf-map for a remaining-PSRAM budget.')
    if errors:print(f'WARN: {len(errors)} malformed BSP(s); see report.')


if __name__=='__main__':
    main()
