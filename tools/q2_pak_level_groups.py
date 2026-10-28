#!/usr/bin/env python3
"""Static Quake II PAK/BSP/MD2 resource groups for RP2350.

Python 3.9+, standard library only. Does not change PAK files or device data.

Usage:
  python q2_pak_level_groups.py --paks "C:\\Quake2\\baseq2\\pak0.pak" "C:\\Quake2\\baseq2\\pak1.pak" "C:\\Quake2\\baseq2\\pak2.pak" --placement assets-placement.csv --game-src src/quake2/game --out q2-analysis
"""
from __future__ import annotations
import argparse
from collections import Counter, defaultdict
import csv
from pathlib import Path
import re
import struct

BSP_LUMPS = 19
FLASH_CHIP_DEFAULT = 4 << 20
FLASH_OFFSET = 900 << 10
PSRAM_BYTES = 8 << 20
ASSET_RE = re.compile(r'models/[^\s"\\]+\.(?:md2|pcx)', re.I)
MODEL_RE = re.compile(r'models/[^\s"\\]+\.md2', re.I)
PAIR_RE = re.compile(r'"((?:\\.|[^"\\])*)"\s*"((?:\\.|[^"\\])*)"', re.S)
FN_RE = re.compile(r'(?m)^\s*(?:(?:static|extern|inline|void|qboolean|int|edict_t|gitem_t|float|char|byte|unsigned|const|model_t|void\s*\*)\s+){1,5}(\w+)\s*\([^;{}]*\)\s*\{')


def align(n: int, unit: int = 16) -> int:
    return (n + unit - 1) // unit * unit


class PakSet:
    def __init__(self, paths):
        self.handles = []
        self.files = {}
        for path in sorted(map(Path, paths), key=lambda p: int(re.search(r'pak(\d+)', p.stem.lower()).group(1)) if re.search(r'pak(\d+)', p.stem.lower()) else -1, reverse=True):
            f = path.open('rb')
            self.handles.append(f)
            f.seek(0, 2)
            sz = f.tell()
            f.seek(0)
            raw = f.read(12)
            if len(raw) != 12:
                raise ValueError(f'{path}: truncated PAK header')
            magic, diroff, dirsize = struct.unpack('<4sii', raw)
            if magic != b'PACK' or diroff < 12 or dirsize < 0 or dirsize % 64 or diroff + dirsize > sz:
                raise ValueError(f'{path}: invalid PAK header/directory')
            f.seek(diroff)
            for i in range(dirsize // 64):
                data = f.read(64)
                name, off, ln = struct.unpack('<56sii', data)
                name = name.split(b'\0', 1)[0].decode('latin-1').replace('\\', '/').lower()
                if off < 0 or ln < 0 or off + ln > sz:
                    raise ValueError(f'{path}: invalid entry {name}')
                if name and name not in self.files:
                    self.files[name] = (f, off, ln, path.name)

    def read(self, name, length=None, offset=0):
        rec = self.files.get(name.lower())
        if not rec:
            return None
        f, off, size, _ = rec
        if offset < 0 or offset > size:
            return None
        size = size - offset if length is None else min(size - offset, length)
        f.seek(off + offset)
        return f.read(size)

    def close(self):
        for f in self.handles:
            f.close()


def parse_entities(raw):
    # The BSP entities lump is a sequence of { "key" "value" } records.
    raw = raw.decode('latin-1', errors='replace')
    out = []
    for match in re.finditer(r'\{([^{}]*)\}', raw, re.S):
        ent = {}
        for m in PAIR_RE.finditer(match.group(1)):
            ent[m.group(1).replace('\\"', '"').lower()] = m.group(2).replace('\\"', '"')
        if ent:
            out.append(ent)
    return out


def parse_bsp(data):
    if len(data) < 8 + 8 * BSP_LUMPS or data[:4] != b'IBSP':
        raise ValueError('invalid BSP header')
    ver = struct.unpack_from('<i', data, 4)[0]
    if ver != 38:
        raise ValueError(f'BSP version {ver}, expected 38')
    lumps = []
    for i in range(BSP_LUMPS):
        at, length = struct.unpack_from('<ii', data, 8 + i * 8)
        if at < 0 or length < 0 or at + length > len(data):
            raise ValueError(f'BSP lump {i} outside file')
        lumps.append((at, length))
    eoff, esize = lumps[0]
    ents = parse_entities(data[eoff:eoff+esize])
    # Texinfo: 2 x vec4 float, int flags, int value, char texture[32], int nexttexinfo.
    texoff, texsize = lumps[5]
    wal = set()
    for i in range(texsize // 76):
        name = data[texoff+i*76+40:texoff+i*76+72].split(b'\0', 1)[0].decode('latin-1').lower()
        if name:
            wal.add('textures/' + name + '.wal')
    return ents, wal, lumps


def md2_info(paks, name):
    raw = paks.read(name, 68)
    if raw is None or len(raw) < 68:
        return None
    h = struct.unpack('<17i', raw)
    if h[0] != 0x32504449 or h[1] != 8:
        return None
    count, off = h[5], h[11]
    fullsize = paks.files[name.lower()][2]
    if count < 0 or count > 32 or off < 68 or off + count * 64 > fullsize:
        return None
    names = paks.read(name, count * 64, off)
    skins = []
    for i in range(count):
        s = names[i*64:(i+1)*64].split(b'\0', 1)[0].decode('latin-1').lower().replace('\\', '/')
        if s:
            skins.append(s)
    framesz = h[4]
    stripped = h[15] >= h[11] + h[5] * 64 and h[15] >= h[12] + h[7] * 4 and h[15] >= h[13] + h[8] * 12 and h[15] >= h[14] + h[10] * framesz and 68 <= h[15] <= fullsize
    return skins, (h[15] if stripped else fullsize)


def pcx_size(paks, name):
    h = paks.read(name, 12)
    if h is None or len(h) < 12 or h[:4] != bytes([10, 5, 1, 8]):
        return None
    xmax, ymax = struct.unpack_from('<HH', h, 8)
    return 8 + (xmax + 1) * (ymax + 1) if xmax < 640 and ymax < 480 else None


def parse_functions(root):
    """Best-effort textual analysis, NOT a C compiler/data-flow evaluator."""
    bodies = {}
    for p in sorted(root.glob('*.c')):
        src = p.read_text(encoding='latin-1')
        # Strip comments, retaining newlines so function patterns stay aligned.
        src = re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: '\n' * m.group().count('\n'), src, flags=re.S)
        for m in FN_RE.finditer(src):
            depth = 0
            for i in range(m.end()-1, len(src)):
                if src[i] == '{':
                    depth += 1
                elif src[i] == '}':
                    depth -= 1
                    if not depth:
                        bodies[m.group(1)] = src[m.end():i]
                        break
    by_class = defaultdict(set)
    # The normal spawn dispatch uses SP_<classname> with some exceptions.
    spawn = root / 'g_spawn.c'
    if spawn.exists():
        text = spawn.read_text(encoding='latin-1')
        for cls, fn in re.findall(r'\{\s*"([a-z][\w]*)"\s*,\s*(SP_\w+)\s*\}', text):
            by_class[cls] |= models_in_function(fn, bodies)
    for fn in bodies:
        if fn.startswith('SP_'):
            by_class[fn[3:]] |= models_in_function(fn, bodies)
    items = root / 'g_items.c'
    if items.exists():
        src = items.read_text(encoding='latin-1')
        # Each itemlist row begins with { "classname", and ends before the next row.
        chunk = src.split('gitem_t\titemlist[]', 1)[-1]
        pat = re.compile(r'\{\s*"((?:item|weapon|ammo|key)_[a-z_0-9]+)"\s*,(.*?)\n\s*\},', re.S)
        for m in pat.finditer(chunk):
            by_class[m.group(1)] |= set(ASSET_RE.findall(m.group(2).lower()))
    # A few engine-wide assets in SP_worldspawn (blaster, gibs).
    common = set(models_in_function('SP_worldspawn', bodies))
    if 'weapon_blaster' in by_class:
        common |= by_class['weapon_blaster']
    known_classes = set(by_class)
    return by_class, common, bodies, known_classes


def models_in_function(fn, bodies):
    seen = set()
    out = set()
    todo = [fn]
    while todo and len(seen) < 30:
        name = todo.pop()
        if name in seen or name not in bodies:
            continue
        seen.add(name)
        body = bodies[name]
        out.update(ASSET_RE.findall(body.lower()))
        # Spawn functions sometimes delegate to another SP_* or a precache helper.
        for called in re.findall(r'\b([A-Za-z_]\w+)\s*\(', body):
            if called in bodies and (called.startswith('SP_') or 'precache' in called.lower()) and called not in seen:
                todo.append(called)
    return out


def load_placement(path):
    out = {}
    if not path:
        return out
    with Path(path).open(encoding='utf-8-sig', newline='') as f:
        for row in csv.DictReader(x for x in f if not x.startswith('#')):
            if row.get('name'):
                out[row['name'].strip('"').lower()] = row
    return out


def write_csv(path, rows, fields):
    with path.open('w', newline='', encoding='utf-8-sig') as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        w.writeheader()
        w.writerows(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument('--paks', nargs='+', required=True, help='PAK files in baseq2 (pak0.pak, pak1.pak, ...)')
    ap.add_argument('--placement', help='assets-placement.csv from the board (optional)')
    ap.add_argument('--game-src', type=Path, help='src/quake2/game of frank-quake2 (strongly recommended)')
    ap.add_argument('--out', type=Path, default=Path('q2-analysis'))
    ap.add_argument('--flash-mb', type=int, default=4)
    ap.add_argument('--firmware-kib', type=int, default=900)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    placement = load_placement(args.placement)
    root = args.game_src
    if not root:
        for c in [Path(__file__).resolve().parent / 'src/quake2/game', Path(__file__).resolve().parent.parent / 'src/quake2/game', Path.cwd() / 'src/quake2/game']:
            if c.is_dir():
                root = c
                break
    by_class, common, bodies, known_classes = parse_functions(root) if root and root.is_dir() else (defaultdict(set), set(), {}, set())
    # Renderer temporary entity models are registered on every map.
    client_root = root.parent / 'client' if root else None
    if client_root and client_root.is_dir():
        ctent = client_root / 'cl_tent.c'
        if ctent.exists():
            src = ctent.read_text(encoding='latin-1')
            match = re.search(r'\bvoid\s+CL_RegisterTEntModels\s*\([^)]*\)\s*\{', src)
            if match:
                depth = 0
                for pos in range(match.end()-1, len(src)):
                    if src[pos] == '{': depth += 1
                    elif src[pos] == '}':
                        depth -= 1
                        if depth == 0:
                            common |= set(ASSET_RE.findall(src[match.end():pos].lower()))
                            break
    capacity = args.flash_mb * (1<<20) - args.firmware_kib * 1024
    if capacity <= 0:
        ap.error('Flash capacity is smaller than firmware reservation')
    paks = PakSet(args.paks)
    try:
        maps = sorted(n for n in paks.files if n.startswith('maps/') and n.endswith('.bsp'))
        if not maps:
            raise ValueError('No Quake II .bsp maps in these PAKs')
        # Use exact size from board CSV when available; otherwise calculate from MD2/PCX headers.
        size_cache = {}
        skin_cache = {}
        def size_for(name):
            name = name.lower()
            if name in size_cache:
                return size_cache[name]
            row = placement.get(name)
            if row and row.get('converted_bytes'):
                sz = int(row['converted_bytes'])
            elif name.endswith('.md2'):
                mi = md2_info(paks, name)
                sz = mi[1] if mi else None
            elif name.endswith('.pcx'):
                sz = pcx_size(paks, name)
            else:
                sz = None
            size_cache[name] = sz
            return sz
        def skins(name):
            if name not in skin_cache:
                mi = md2_info(paks, name)
                skin_cache[name] = mi[0] if mi else []
            return skin_cache[name]

        dep_rows = []
        sum_rows = []
        class_unknown = defaultdict(set)
        map_assets = {}
        reasons_map = {}
        wal_by_map = {}
        transitions = []
        for map_path in maps:
            short = map_path[len('maps/'):-len('.bsp')]
            raw = paks.read(map_path)
            try:
                entities, wal, lumps = parse_bsp(raw)
            except ValueError as e:
                print(f'WARN: {map_path}: {e}')
                continue
            wal_by_map[short] = wal
            for ent in entities:
                if ent.get('classname', '').lower() == 'target_changelevel' and ent.get('map'):
                    target = ent['map'].strip().lower()
                    if target.startswith('*'): target = target[1:]
                    target = target.split('$', 1)[0].split('/', 1)[0]
                    if target.endswith('.bsp'): target = target[:-4]
                    transitions.append({'from_map':short,'to_map':target,'in_pak':int('maps/'+target+'.bsp' in paks.files),
                                        'targetname':ent.get('targetname','')})
            direct = defaultdict(set)
            for model in common:
                direct[model].add('global:SP_worldspawn/Blaster')
            classes = Counter()
            for ent in entities:
                cls = ent.get('classname', '(unknown)').lower()
                classes[cls] += 1
                for key, val in ent.items():
                    v = val.lower().replace('\\', '/')
                    if v.endswith(('.md2', '.pcx')) and '/' in v:
                        direct[v].add(f'entity:{cls}:{key}')
                models = by_class.get(cls)
                if models:
                    for name in models:
                        direct[name].add(f'game:SP_{cls}')
                elif cls not in known_classes and cls not in ('worldspawn', '(unknown)'):
                    class_unknown[cls].add(short)
            for name in list(direct):
                if name.endswith('.md2'):
                    for skin in skins(name):
                        direct[skin].add('md2-skin:' + name)
            map_assets[short] = set(direct)
            reasons_map[short] = direct
            known_flash = 0
            unknown = 0
            hypothetical_flash = 32
            resident = 0
            converted = 0
            count_flash = 0
            for name in sorted(direct):
                size = size_for(name)
                row = placement.get(name)
                in_flash = row is not None and row.get('status') == 'PLACE'
                known = size is not None
                if not known:
                    unknown += 1
                else:
                    converted += size
                    hypothetical_flash += align(size) + 64
                    if in_flash:
                        known_flash += size
                        count_flash += 1
                    else:
                        resident += size
                dep_rows.append({
                    'map':short, 'name':name, 'kind':name.rsplit('.',1)[-1].upper(),
                    'converted_bytes':size if known else '',
                    'allocated_bytes':align(size)+64 if known else '',
                    'in_current_flash':int(in_flash),
                    'psram_min_bytes_from_asset':size if known and not in_flash else '',
                    'source':';'.join(sorted(direct[name])),
                })
            sum_rows.append({
                'map':short, 'entities':len(entities), 'class_count':len(classes),
                'assets':len(direct), 'assets_known':len(direct)-unknown,
                'assets_unknown_size':unknown, 'md2_pcx_total_bytes':converted,
                'in_current_flash_bytes':known_flash,
                'psram_md2_pcx_min_bytes':resident,
                'psram_mb_lower_bound':f'{resident / (1<<20):.3f}',
                'full_map_flash_cost_bytes':hypothetical_flash,
                'full_map_fits_flash':int(unknown == 0 and hypothetical_flash <= capacity),
                'bsp_file_bytes':len(raw),
                'bsp_wal_files':len(wal),
                'bsp_wal_disk_bytes':sum(paks.files[w][2] for w in wal if w in paks.files),
                'unresolved_classes':sum(1 for c in classes if c not in known_classes and c not in ('worldspawn','(unknown)')),
                'over_8mb_asset_only':int(resident > PSRAM_BYTES),
            })
        usage = defaultdict(set)
        for k, vals in map_assets.items():
            for n in vals:
                usage[n].add(k)
        # Potential shared group: all-map intersection, not necessarily resident at runtime.
        common_all = set.intersection(*map_assets.values()) if map_assets else set()
        use_rows = []
        for name, used_on in sorted(usage.items(), key=lambda t: (-len(t[1]), t[0])):
            row = placement.get(name)
            size = size_for(name)
            use_rows.append({'name':name,'maps_using':len(used_on),'map_fraction':f'{len(used_on)/len(map_assets):.4f}' if map_assets else '0',
                             'converted_bytes':size if size is not None else '',
                             'flash_cost_bytes':align(size)+64 if size is not None else '',
                             'in_current_flash':int(row is not None and row.get('status')=='PLACE'),
                             'group':'COMMON_ALL_MAPS' if name in common_all else 'MAP_DEPENDENT',
                             'maps':';'.join(sorted(used_on))})
        write_csv(args.out/'map-transitions.csv', transitions, ['from_map','to_map','in_pak','targetname'])
        write_csv(args.out/'map-assets.csv', dep_rows, ['map','name','kind','converted_bytes','allocated_bytes','in_current_flash','psram_min_bytes_from_asset','source'])
        write_csv(args.out/'map-summary.csv', sorted(sum_rows,key=lambda x:-x['psram_md2_pcx_min_bytes']),
                  ['map','entities','class_count','assets','assets_known','assets_unknown_size','md2_pcx_total_bytes','in_current_flash_bytes','psram_md2_pcx_min_bytes','psram_mb_lower_bound','full_map_flash_cost_bytes','full_map_fits_flash','bsp_file_bytes','bsp_wal_files','bsp_wal_disk_bytes','unresolved_classes','over_8mb_asset_only'])
        write_csv(args.out/'resource-usage.csv', use_rows, ['name','maps_using','map_fraction','converted_bytes','flash_cost_bytes','in_current_flash','group','maps'])
        unknown_rows = [{'classname':cl,'maps':';'.join(sorted(mlist)),'map_count':len(mlist)} for cl,mlist in sorted(class_unknown.items())]
        write_csv(args.out/'unresolved-classnames.csv', unknown_rows, ['classname','map_count','maps'])
        with (args.out/'report.txt').open('w',encoding='utf-8') as f:
            print('Quake II offline PAK/BSP/MD2 groups',file=f)
            print(f'Maps: {len(map_assets)}; PAK entries: {len(paks.files)}; game C functions indexed: {len(bodies)}', file=f)
            print(f'Flash: {args.flash_mb} MiB; firmware reserve: {args.firmware_kib} KiB; asset capacity: {capacity} bytes',file=f)
            print(f'Placement CSV: {args.placement or "NOT PROVIDED (all in_current_flash fields = 0)"}',file=f)
            print(f'Game sources: {root if bodies else "NOT AVAILABLE - dependencies incomplete"}',file=f)
            print(f'Globally shared model+skin resources (intersection): {len(common_all)}',file=f)
            print(f'BSP changelevel transitions: {len(transitions)}',file=f)
            print(f'Complete known converted model/skin set fits the flash per-map: {sum(r["full_map_fits_flash"] for r in sum_rows)} of {len(sum_rows)} (unknown sizes prevent confirmation)',file=f)
            total_common = sum(size_for(n) or 0 for n in common_all)
            print(f'Intersection converted bytes: {total_common}',file=f)
            print('Largest per-map known MD2+PCX PSRAM demands with the current Flash placement:',file=f)
            for r in sorted(sum_rows,key=lambda x:-x['psram_md2_pcx_min_bytes'])[:25]:
                print(f'  {r["map"]:18s} {r["psram_md2_pcx_min_bytes"]:9,d} B ({r["psram_mb_lower_bound"]} MiB), per-map flash {r["full_map_flash_cost_bytes"]:9,d} B, assets {r["assets"]}, unknown sizes {r["assets_unknown_size"]}, unknown classes {r["unresolved_classes"]}',file=f)
            print('',file=f)
            print('LIMITATIONS: This is a lower bound for MD2/PCX bytes, NOT the peak PSRAM demand.',file=f)
            print('BSP geometry expansion, WAL textures/mipmaps, game state, heap fragmentation, sound,',file=f)
            print('runtime level transitions/spawn conditions, dynamic entity spawns, client models,',file=f)
            print('and allocations outside these assets are not included.',file=f)
            print('C source cross-function dynamic expressions cannot be exhaustively evaluated.',file=f)
            print('A result below 8 MiB DOES NOT prove that a level fits 8 MiB PSRAM.',file=f)
        print(f'Analyzed {len(map_assets)} maps; {len(paks.files)} PAK entries; {len(bodies)} C functions.')
        print(f'Reports saved to: {args.out.resolve()} (map-assets.csv, map-summary.csv, resource-usage.csv, map-transitions.csv, unresolved-classnames.csv, report.txt)')
        if sum_rows:
            worst=max(sum_rows,key=lambda x:x['psram_md2_pcx_min_bytes'])
            print(f'Largest per-map MD2/PCX PSRAM lower bound: {worst["map"]}: {worst["psram_md2_pcx_min_bytes"]:,} bytes.')
        print('CAUTION: this is NOT the total or peak PSRAM use. Read report.txt.')
    finally:
        paks.close()


if __name__ == '__main__':
    main()
