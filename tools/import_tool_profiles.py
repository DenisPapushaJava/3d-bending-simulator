#!/usr/bin/env python3
"""Prepare a private catalogue from XY DXF contours; standard library only.

No source drawing is bundled or uploaded. DWG is deliberately not an in-process
runtime dependency. The associated DXFs provide these sections; DWG order sheets
must be audited separately before adding catalogue facts.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys

TOLERANCE = 1e-5


def point_distance(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def segment(a, b, bulge=0):
    a, b = list(a[:2]), list(b[:2])
    center = None
    sweep = 0
    if abs(bulge) > 1e-14:
        dx, dy = b[0] - a[0], b[1] - a[1]
        center = [(a[0] + b[0]) / 2 - dy * (1 - bulge * bulge) / (4 * bulge),
                  (a[1] + b[1]) / 2 + dx * (1 - bulge * bulge) / (4 * bulge)]
        sweep = 4 * math.atan(bulge)
    return dict(start=a, end=b, center=center, sweepRadians=sweep)


def parse_dxf(path):
    raw = path.read_bytes()
    if not 0 < len(raw) <= 4 * 1024 * 1024:
        raise ValueError('DXF must be between 1 byte and 4 MiB')
    # All interpreted tags are ASCII numeric values. Layer names are opaque IDs.
    lines = raw.decode('latin1').splitlines()
    if len(lines) % 2 or len(lines) > 400000:
        raise ValueError('Malformed/oversized ASCII DXF')
    tags = [(int(lines[i].strip()), lines[i + 1].strip()) for i in range(0, len(lines), 2)]
    headers, records = {}, []
    section, record = None, None
    for i, (code, value) in enumerate(tags):
        if code == 0:
            if record is not None:
                records.append(record)
                record = None
            if value == 'SECTION':
                section = tags[i + 1][1]
            elif value == 'ENDSEC':
                section = None
            elif section == 'ENTITIES':
                record = {'type': value, 'tags': []}
        elif section == 'ENTITIES' and record is not None:
            record['tags'].append((code, value))
        elif section == 'HEADER' and code == 9:
            headers[value] = tags[i + 1][1]
    edges, z_values = [], []
    poly, vertices = None, []

    def number(r, code, default=0):
        v = float(next((v for c, v in r['tags'] if c == code), default))
        if not math.isfinite(v) or abs(v) > 1e6:
            raise ValueError('Non-finite or oversized coordinate')
        return v

    def plane(r):
        if [number(r, 210), number(r, 220), number(r, 230, 1)] != [0, 0, 1]:
            raise ValueError('Only default XY extrusion is supported')
        if number(r, 39) != 0:
            raise ValueError('Entity thickness is unsupported')

    for r in records:
        typ = r['type']
        if typ == 'LINE':
            plane(r)
            edges.append(segment([number(r, 10), number(r, 20)], [number(r, 11), number(r, 21)]))
            z_values += [number(r, 30), number(r, 31)]
        elif typ == 'ARC':
            plane(r)
            c = [number(r, 10), number(r, 20)]
            radius = number(r, 40)
            start = math.radians(number(r, 50))
            sweep = math.radians((number(r, 51) - number(r, 50)) % 360)
            if radius <= 0 or sweep <= 0:
                raise ValueError('Degenerate arc')
            at = lambda a: [c[0] + radius * math.cos(a), c[1] + radius * math.sin(a)]
            edges.append(dict(start=at(start), end=at(start + sweep), center=c, sweepRadians=sweep))
            z_values.append(number(r, 30))
        elif typ == 'POLYLINE':
            plane(r)
            if poly is not None or int(number(r, 70)) & ~1:
                raise ValueError('Only a simple 2D polyline is supported')
            poly, vertices = r, []
            z_values.append(number(r, 30))
        elif typ == 'VERTEX' and poly is not None:
            vertices.append([number(r, 10), number(r, 20), number(r, 42)])
            z_values.append(number(r, 30))
        elif typ == 'SEQEND' and poly is not None:
            count = len(vertices) if int(number(poly, 70)) & 1 else len(vertices) - 1
            for i in range(count):
                a, b = vertices[i], vertices[(i + 1) % len(vertices)]
                if point_distance(a, b) > 1e-8:
                    edges.append(segment(a, b, a[2]))
            poly = None
        elif typ not in ('TEXT', 'MTEXT', 'POINT', 'DIMENSION'):
            raise ValueError('Unsupported DXF entity: ' + typ)
        if len(edges) > 2048:
            raise ValueError('Too many edges')
    if poly is not None or not z_values or max(z_values) - min(z_values) > 1e-8:
        raise ValueError('Missing/incomplete or non-planar XY geometry')
    return headers, edges


def ordered_contour(edges):
    nodes, adjacency, ends = [], [], []

    def node(p):
        for i, q in enumerate(nodes):
            if point_distance(p, q) <= TOLERANCE:
                return i
        nodes.append(p)
        adjacency.append([])
        return len(nodes) - 1

    for i, edge in enumerate(edges):
        a, b = node(edge['start']), node(edge['end'])
        if a == b:
            raise ValueError('Degenerate edge')
        ends.append((a, b))
        adjacency[a].append(i)
        adjacency[b].append(i)
    unseen, closed = set(range(len(nodes))), []
    while unseen:
        group, stack = set(), [next(iter(unseen))]
        while stack:
            n = stack.pop()
            if n in group:
                continue
            group.add(n)
            for i in adjacency[n]:
                stack += list(ends[i])
        unseen -= group
        if all(len(adjacency[n]) == 2 for n in group):
            closed.append(group)
    if len(closed) != 1:
        raise ValueError('Expected exactly one closed unbranched contour; found ' + str(len(closed)))
    first = min(closed[0])
    current, used, ordered = first, set(), []
    while True:
        options = [i for i in adjacency[current] if i not in used]
        if not options:
            break
        i = options[0]
        used.add(i)
        edge = dict(edges[i])
        if ends[i][0] != current:
            edge['start'], edge['end'] = edge['end'], edge['start']
            edge['sweepRadians'] = -edge['sweepRadians']
        current = ends[i][1] if ends[i][0] == current else ends[i][0]
        ordered.append(edge)
    if current != first or not 2 <= len(ordered) <= 512:
        raise ValueError('Invalid/oversized contour')
    # Normalize translation only: preserve source scale, axes, all analytic arcs.
    origin = ordered[0]['start'][:]
    for edge in ordered:
        for key in ('start', 'end', 'center'):
            if edge[key] is not None:
                edge[key] = [edge[key][j] - origin[j] for j in (0, 1)]
    return ordered, len(edges) - len(ordered)


def import_profiles(catalog, directory, millimeters=()):
    report = []
    for path in sorted(directory.glob('*')):
        if path.suffix.lower() != '.dxf':
            continue
        headers, edges = parse_dxf(path)
        contour, ignored = ordered_contour(edges)
        matches = [t for t in catalog['tools']
                   if re.fullmatch(re.escape(path.stem.upper()) + '[SM]?', t['code'].upper())]
        if not matches:
            raise ValueError('Add a catalogue inventory record before importing ' + path.name)
        drawing = dict(sourceFile=path.name, sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                       millimeters=headers.get('$INSUNITS') == '4' or path.stem.upper() in millimeters,
                       edges=contour)
        for tool in matches:
            tool['drawing'] = drawing
        report.append(dict(file=path.name, tools=[t['code'] for t in matches],
                           millimeters=drawing['millimeters'], edges=len(contour), ignoredEdges=ignored))
    catalog['version'] = 2
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--catalog', type=Path, default=Path(__file__).resolve().parents[1] / 'src/project/data/tool-catalog.json')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--replace', action='store_true')
    parser.add_argument('--millimeters', action='append', default=[], metavar='CODE',
                        help='Explicit operator confirmation of units for a drawing, e.g. ST2263')
    args = parser.parse_args()
    if not args.directory.is_dir():
        parser.error('Input directory does not exist')
    if args.output.exists() and not args.replace:
        parser.error('Output exists; use --replace explicitly')
    catalog = json.loads(args.catalog.read_text(encoding='utf-8'))
    report = import_profiles(catalog, args.directory, {code.upper() for code in args.millimeters})
    if not report:
        parser.error('No DXF profiles found')
    data = json.dumps(catalog, ensure_ascii=False, indent=2, allow_nan=False) + '\n'
    if len(data.encode('utf-8')) > 4 * 1024 * 1024:
        parser.error('Catalogue exceeds 4 MiB')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + '.tmp')
    temporary.write_text(data, encoding='utf-8')
    temporary.replace(args.output)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    dwg = [p.name for p in args.directory.iterdir() if p.suffix.lower() == '.dwg']
    if dwg:
        print('DWG order sheets require separate inspection; not treated as tool article codes: ' + ', '.join(sorted(dwg)), file=sys.stderr)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, IndexError, KeyError) as error:
        sys.exit(str(error))
