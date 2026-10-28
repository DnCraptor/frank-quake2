#!/usr/bin/env python3
"""Static Quake II BSP PVS -> potential simultaneous WAL demand.

Requires q2_pak_level_groups.py and q2_pak_memory.py in the same directory.
Reads only the Quake II PAK files; it does not touch the device or Flash image.

Example from the repository root:
    python tools/q2_pvs_budget.py --paks C:/Q2/baseq2/pak0.pak \
        C:/Q2/baseq2/pak1.pak C:/Q2/baseq2/pak2.pak \
        --memory q2-memory/map-memory.csv \
        --analysis q2-analysis \
        --selection tools/q2_4mb_flash_selection.csv \
        --out q2-pvs

A PVS lists potentially visible leaves, NOT the actual textures drawn in a
frame. It excludes dynamic alias models, screen UI, skybox PCXs and transient
allocations. Moving brush models are treated conservatively as always visible.
The 'PVS headroom' is hypothetical: it assumes all map MD2/PCX remain loaded
and that all WALs not in the selected PVS were evicted. The engine does not
necessarily evict all of these at a cluster transition (recent-frame rule).
"""
from __future__ import annotations

import argparse
import csv
from pathlib import Path
import struct
from collections import defaultdict

from q2_pak_level_groups import PakSet, parse_bsp, align
from q2_pak_memory import wal_memory

# Quake II BSP v38 lump indices (qfiles.h).
L_VIS = 3
L_FACES = 6
L_LEAFS = 8
L_LEAFFACES = 9
L_MODELS = 13
L_TEXINFO = 5
FORCE_MIP0 = 0x04 | 0x08 | 0x10 | 0x20 | 0x40


def read_csv(path):
    if not path:
        return {}
    with Path(path).open('r', newline='', encoding='utf-8-sig') as stream:
        return {r['map']: r for r in csv.DictReader(stream)}


def selected_model_bytes(analysis_path, selection_path, memory):
    """Recompute a Flash scenario from the static map/asset inventory.

    Uses exactly the same converted-byte accounting as q2_pak_memory.py.
    No prior q2-memory-assessment report is required.
    """
    with Path(selection_path).open('r', newline='', encoding='utf-8-sig') as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames or not {'name', 'in_4MB_flash'} <= set(reader.fieldnames):
            raise ValueError(f'{selection_path}: expected name,in_4MB_flash columns')
        selected = set()
        for row in reader:
            if row['in_4MB_flash'].strip() == '1':
                selected.add(row['name'].strip().lower())
    if not selected:
        raise ValueError(f'{selection_path}: no selected Flash assets')

    assets_path = Path(analysis_path) / 'map-assets.csv'
    per_map = defaultdict(list)
    with assets_path.open('r', newline='', encoding='utf-8-sig') as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames or not {'map', 'name', 'converted_bytes', 'in_current_flash'} <= set(reader.fieldnames):
            raise ValueError(f'{assets_path}: incomplete map-assets.csv columns')
        for row in reader:
            per_map[row['map']].append(row)

    computed = {}
    errors = []
    for name, baseline in memory.items():
        if name not in per_map:
            errors.append(f'{name}: absent in {assets_path}')
            continue
        assets = per_map[name]
        from_map = sum(int(a['converted_bytes'] or 0) for a in assets
                       if a['in_current_flash'] != '1')
        from_memory = int(baseline['model_skin_current_bytes'])
        if from_map != from_memory:
            errors.append(f'{name}: current asset bytes disagree: '
                          f'map-assets={from_map}, map-memory={from_memory}')
            continue
        selected_resident = sum(int(a['converted_bytes'] or 0) for a in assets
                                if a['name'].lower() not in selected)
        free_current = baseline.get('left_current_no_sound_bytes', '')
        if free_current == '':
            errors.append(f'{name}: --memory lacks PSRAM headroom (supply --elf-map to q2_pak_memory.py)')
            continue
        computed[name] = int(free_current) + from_memory - selected_resident
    if errors:
        raise ValueError('Cannot compare Flash selection:\n  ' + '\n  '.join(errors[:10]))
    return computed, len(selected)


def write_csv(path, rows, fields):
    with Path(path).open('w', newline='', encoding='utf-8-sig') as stream:
        out = csv.DictWriter(stream, fieldnames=fields)
        out.writeheader()
        out.writerows(rows)


def lump_data(bsp, lumps, idx):
    off, size = lumps[idx]
    return memoryview(bsp)[off:off + size]


def decode_pvs(vis, offset, nclusters):
    """RLE used by Mod_DecompressVis in ref_soft/r_model.c.

    Missing PVS (-1) means all clusters potentially visible. Bad RLE fails
    closed (all visible), rather than producing an artificially small budget.
    """
    outlen = (nclusters + 7) // 8
    if offset < 0:
        return bytes([255]) * outlen, False
    out = bytearray()
    pos = offset
    try:
        while len(out) < outlen:
            if pos >= len(vis):
                raise ValueError('truncated PVS')
            c = vis[pos]
            pos += 1
            if c:
                out.append(c)
            else:
                if pos >= len(vis):
                    raise ValueError('truncated PVS zero run')
                n = vis[pos]
                pos += 1
                if not n:
                    raise ValueError('zero-length PVS run')
                out.extend(bytes(min(n, outlen - len(out))))
        return bytes(out[:outlen]), True
    except ValueError:
        return bytes([255]) * outlen, False


def visit_bits(mask):
    """Integer-index iterator (handles large BSP texture/cluster bitsets)."""
    while mask:
        bit = mask & -mask
        yield bit.bit_length() - 1
        mask ^= bit


def parse_bsp_visibility(paks, bsp, lumps, mipcap):
    texblob = lump_data(bsp, lumps, L_TEXINFO)
    faceblob = lump_data(bsp, lumps, L_FACES)
    leafblob = lump_data(bsp, lumps, L_LEAFS)
    leafaces = lump_data(bsp, lumps, L_LEAFFACES)
    models = lump_data(bsp, lumps, L_MODELS)
    vis = lump_data(bsp, lumps, L_VIS)
    sizes = (76, 20, 28, 2, 48)
    blobs = (texblob, faceblob, leafblob, leafaces, models)
    for blob, unit in zip(blobs, sizes):
        if len(blob) % unit:
            raise ValueError(f'BSP lump length {len(blob)} not divisible by {unit}')

    # Convert each texinfo into a WAL name, keeping the most demanding mip if
    # a file is used by several different texinfo entries.
    tex = []
    wal_mip0 = {}
    for i in range(len(texblob) // 76):
        at = i * 76
        flags = struct.unpack_from('<i', texblob, at + 32)[0]
        name = bytes(texblob[at + 40:at + 72]).split(b'\0', 1)[0].decode('latin-1').lower().replace('\\', '/')
        next_tex = struct.unpack_from('<i', texblob, at + 72)[0]
        name = ('textures/' + name + '.wal') if name else ''
        mip0 = mipcap == 0 or bool(flags & FORCE_MIP0)
        if name:
            wal_mip0[name] = wal_mip0.get(name, False) or mip0
        tex.append((name, next_tex))

    wals = sorted(wal_mip0)
    walbit = {name: 1 << i for i, name in enumerate(wals)}
    wal_sizes = []
    missing = []
    for name in wals:
        size = wal_memory(paks, name, wal_mip0[name])
        if size is None:
            missing.append(name)
            wal_sizes.append(0)
        else:
            wal_sizes.append(align(size) + 16)  # 32-bit q_arena block header
    wal_all = sum(wal_sizes)

    # Animation chains can use subsequent WALs without a change of PVS.
    texmask = []
    for i in range(len(tex)):
        mask, seen, j = 0, set(), i
        while 0 <= j < len(tex) and j not in seen:
            seen.add(j)
            name, next_tex = tex[j]
            if name:
                mask |= walbit[name]
            # Mirrors r_model.c:Mod_LoadTexinfo (nexttexinfo > 0 only).
            j = next_tex if next_tex > 0 else -1
        texmask.append(mask)

    facemasks = []
    for i in range(len(faceblob) // 20):
        tex_index = struct.unpack_from('<h', faceblob, i * 20 + 10)[0]
        facemasks.append(texmask[tex_index] if 0 <= tex_index < len(texmask) else 0)
    leaf_indices = struct.unpack_from('<' + 'H' * (len(leafaces) // 2), leafaces) if leafaces else ()

    # Brush submodels (doors/platforms/etc) are potentially visible in any PVS;
    # keeping their textures in every set is conservative.
    inline_mask = 0
    for i in range(1, len(models) // 48):
        first, n = struct.unpack_from('<ii', models, i * 48 + 40)
        if first < 0 or n < 0 or first + n > len(facemasks):
            raise ValueError('bad inline submodel face range')
        for fi in range(first, first + n):
            inline_mask |= facemasks[fi]

    if len(vis) < 4:
        nclusters, vis_fallback = 0, True
    else:
        nclusters = struct.unpack_from('<i', vis, 0)[0]
        vis_fallback = nclusters <= 0 or nclusters > 65536 or 4 + nclusters * 8 > len(vis)
    if vis_fallback:
        return {'clusters': 0, 'fallback': True, 'missing': missing,
                'wal_total': wal_all, 'wal_count': len(wals),
                'pvs_peak': wal_all, 'peak_cluster': -1,
                'peak_visible_clusters': 0, 'pvs_rows': []}

    cluster_faces = [0] * nclusters
    for i in range(len(leafblob) // 28):
        at = i * 28
        cluster = struct.unpack_from('<h', leafblob, at + 4)[0]
        first, nfaces = struct.unpack_from('<HH', leafblob, at + 20)
        if cluster < 0 or cluster >= nclusters:
            continue
        if first + nfaces > len(leaf_indices):
            raise ValueError('bad leaf face range')
        mask = cluster_faces[cluster]
        for j in range(first, first + nfaces):
            fi = leaf_indices[j]
            if fi < len(facemasks):
                mask |= facemasks[fi]
        cluster_faces[cluster] = mask

    pvs_rows = []
    peak = -1
    peak_cluster = -1
    peak_visible_clusters = 0
    decode_failures = 0
    for c in range(nclusters):
        pvs_off = struct.unpack_from('<i', vis, 4 + c * 8)[0]
        data, valid = decode_pvs(vis, pvs_off, nclusters)
        if not valid:
            decode_failures += 1
        wal_mask = inline_mask | cluster_faces[c]
        visible = 0
        for byte_idx, byte in enumerate(data):
            while byte:
                bit = byte & -byte
                j = byte_idx * 8 + bit.bit_length() - 1
                if j < nclusters:
                    visible += 1
                    wal_mask |= cluster_faces[j]
                byte ^= bit
        resident = sum(wal_sizes[i] for i in visit_bits(wal_mask))
        row = {'cluster': c, 'visible_clusters': visible,
               'potential_wal_count': wal_mask.bit_count(),
               'potential_wal_bytes': resident,
               'pvs_valid': int(valid)}
        pvs_rows.append(row)
        if resident > peak:
            peak, peak_cluster, peak_visible_clusters = resident, c, visible
    return {'clusters': nclusters, 'fallback': decode_failures == nclusters,
            'decode_fallbacks': decode_failures, 'missing': missing,
            'wal_total': wal_all, 'wal_count': len(wals),
            'pvs_peak': peak, 'peak_cluster': peak_cluster,
            'peak_visible_clusters': peak_visible_clusters, 'pvs_rows': pvs_rows}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--paks', nargs='+', required=True)
    ap.add_argument('--memory', type=Path, help='q2-memory/map-memory.csv')
    ap.add_argument('--comparison', type=Path, help='optional precomputed map-memory-comparison.csv')
    ap.add_argument('--analysis', type=Path, help='q2-analysis directory, required with --selection')
    ap.add_argument('--selection', type=Path, help='Flash selection CSV; compute scenario locally from --analysis and --memory')
    ap.add_argument('--comparison-field', default='free_102_no_sound_bytes')
    ap.add_argument('--mipcap', type=int, choices=[0,1], default=1)
    ap.add_argument('--arena-reserve-kib', type=int, default=192)
    ap.add_argument('--out', type=Path, default=Path('q2-pvs'))
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    if args.selection and args.comparison:
        ap.error('--selection and --comparison are alternatives; specify only one')
    if args.selection and (not args.analysis or not args.memory):
        ap.error('--selection requires both --analysis and --memory')
    for label, path in [('--memory', args.memory), ('--comparison', args.comparison),
                        ('--selection', args.selection)]:
        if path is not None and not path.is_file():
            ap.error(f'{label} file not found: {path}. Use --analysis q2-analysis '
                     'and --selection tools/q2_4mb_flash_selection.csv '
                     'instead of the old --comparison argument.')
    if args.analysis and not (args.analysis / 'map-assets.csv').is_file():
        ap.error(f'missing {args.analysis / "map-assets.csv"}')
    baseline = read_csv(args.memory)
    comparison = read_csv(args.comparison)
    if comparison and args.comparison_field not in next(iter(comparison.values())):
        ap.error(f'Column not present in --comparison: {args.comparison_field}')
    computed_selected = {}
    selected_count = 0
    if args.selection:
        try:
            computed_selected, selected_count = selected_model_bytes(args.analysis, args.selection, baseline)
        except (OSError, KeyError, ValueError) as e:
            ap.error(str(e))

    paks = PakSet(args.paks)
    result_rows = []
    cluster_rows = []
    notes = []
    try:
        maps = sorted(k for k in paks.files if k.startswith('maps/') and k.endswith('.bsp'))
        for path in maps:
            name = path[5:-4]
            raw = paks.read(path)
            try:
                _, _, lumps = parse_bsp(raw)
                d = parse_bsp_visibility(paks, raw, lumps, args.mipcap)
            except (ValueError, struct.error) as e:
                notes.append(f'{name}: ERROR {e}; excluded from PVS analysis')
                continue
            old = baseline.get(name, {})
            comp = comparison.get(name, {})
            if old and args.mipcap == 1 and old.get('wal_resident_bytes') and int(old['wal_resident_bytes']) != d['wal_total']:
                notes.append(f'{name}: WAL total mismatch: memory.csv {old["wal_resident_bytes"]} vs PVS {d["wal_total"]}; headroom columns may be invalid')
            base_free = None
            if comp:
                base_free = int(comp[args.comparison_field])
            elif name in computed_selected:
                base_free = computed_selected[name]
            elif old and args.mipcap == 1 and old.get('left_current_no_sound_bytes', '') != '':
                base_free = int(old['left_current_no_sound_bytes'])
            # With the same mipcap and asset selection, this uses old resident
            # sum of ALL WAL as an upper scenario, and replaces it with
            # potentially visible WAL. Does NOT guarantee they are evicted.
            saved = d['wal_total'] - d['pvs_peak']
            pvs_free = (base_free + saved) if base_free is not None else None
            reserve = args.arena_reserve_kib * 1024
            row = dict(map=name, clusters=d['clusters'], pvs_fallback=int(d['fallback']),
                       pvs_decode_fallbacks=d.get('decode_fallbacks', 0),
                       wal_count=d['wal_count'], missing_wal=len(d['missing']),
                       all_wal_bytes=d['wal_total'], pvs_wal_peak_bytes=d['pvs_peak'],
                       potentially_evictable_wal_bytes=saved,
                       worst_pvs_cluster=d['peak_cluster'],
                       worst_pvs_visible_clusters=d['peak_visible_clusters'],
                       scenario_headroom_all_wal_bytes=base_free if base_free is not None else '',
                       scenario_headroom_pvs_bytes=pvs_free if pvs_free is not None else '',
                       non_wal_relief_to_fit_bytes=max(0,-pvs_free) if pvs_free is not None else '',
                       non_wal_relief_for_192k_reserve_bytes=max(0,reserve-pvs_free) if pvs_free is not None else '')
            result_rows.append(row)
            for cr in d['pvs_rows']:
                cluster_rows.append({'map': name, **cr})
            print(f'{name:16s} PVS {d["clusters"]:5d}, WAL all={d["wal_total"]:8d}, peak={d["pvs_peak"]:8d}, freed~={saved:8d}, scenario_free={pvs_free}')
        if (args.comparison or args.selection) and args.mipcap != 1:
            notes.append('WARNING: PSRAM headroom was computed with original mipcap; '
                         'rerun q2_pak_memory.py for the requested mipcap.')
        outfields = ['map','clusters','pvs_fallback','pvs_decode_fallbacks','wal_count',
                     'missing_wal','all_wal_bytes','pvs_wal_peak_bytes','potentially_evictable_wal_bytes',
                     'worst_pvs_cluster','worst_pvs_visible_clusters',
                     'scenario_headroom_all_wal_bytes','scenario_headroom_pvs_bytes',
                     'non_wal_relief_to_fit_bytes','non_wal_relief_for_192k_reserve_bytes']
        write_csv(args.out/'pvs-map-memory.csv',result_rows,outfields)
        write_csv(args.out/'pvs-clusters.csv',cluster_rows,
                  ['map','cluster','visible_clusters','potential_wal_count','potential_wal_bytes','pvs_valid'])
        with (args.out/'report.txt').open('w',encoding='utf-8') as f:
            print('Quake II BSP PVS / WAL offline assessment',file=f)
            print('PVS = potential spatial visibility, NOT exact simultaneously resident images.',file=f)
            print('All brush inline submodel faces assumed visible at every cluster.',file=f)
            print('All animation-chain frames included; one worst mip per WAL.',file=f)
            print('Clusters are sampled independently; the engine can retain recent textures',file=f)
            print('from a different PVS and dynamically loaded models are not included here.',file=f)
            if args.selection:
                print(f'Flash scenario recomputed from {args.selection} ({selected_count} selected assets)', file=f)
                print(f'Asset inventory: {args.analysis / "map-assets.csv"}', file=f)
            elif args.comparison:
                print(f'Headroom sourced from {args.comparison} column {args.comparison_field}', file=f)
            else:
                print('Headroom uses CURRENT Flash allocation from --memory.', file=f)
            print('Scenario headroom requires the same PAK data and mipcap as --memory.', file=f)
            print('Missing WALs are reported; they do not contribute a guessed size.',file=f)
            for r in sorted(result_rows,key=lambda r:r['scenario_headroom_pvs_bytes'] if r['scenario_headroom_pvs_bytes']!='' else 10**12)[:15]:
                print(f'{r["map"]:16s} all={r["all_wal_bytes"]:8d} PVS-peak={r["pvs_wal_peak_bytes"]:8d} '
                      f'headroom-pvs={r["scenario_headroom_pvs_bytes"]}',file=f)
            print('\n'+'\n'.join(notes),file=f)
        print(f'Wrote {len(result_rows)} map rows and {len(cluster_rows)} cluster rows to {args.out}')
    finally:
        paks.close()


if __name__ == '__main__':
    main()
