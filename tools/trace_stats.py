#!/usr/bin/env python3
"""Sum up frame traces (OPENCMR2_GFX_TRACE) for the worst-case numbers.

Each trace is named after its run: a path like runs/<scenario>/gfx.trace, as
tests/runtime_scenarios.py --trace writes them. Prints the maxima per frame,
the most live texture and vertex buffer memory, and the fragment key and
pipeline sets. --variants writes the key sets for the GXM shader variants,
with the arguments keys each fragment key drew with.
"""
import argparse
from pathlib import Path
import re

FORMATS = ['BGRX8', 'BGRA8', 'BC1', 'BC3', 'BUMP_DUDVL']
FRAME = re.compile(r'^F (\d+) v=(\d+) i=(\d+) c=(\d+) s=(\d+) b=(\d+)/(\d+)$')
DRAW = re.compile(r' k=([0-9a-f]+) p=([0-9a-f]+)(?: a=([0-9a-f]+))?$')
FRAME_FIELDS = ['vertex bytes', 'indices', 'commands', 'states', 'buffers drawn', 'buffer bytes drawn']


def triple(fragment, pipeline):
    """(fragment key, blend pair, vertex program): the GXM programs a draw needs."""
    blend = 'off' if not pipeline >> 4 & 1 else f'{pipeline >> 5 & 15}/{pipeline >> 9 & 15}'
    vertex = 'tl' if pipeline & 1 else '3d'
    return f'{fragment:09x} {blend} {vertex}'


def read(path, name, stats):
    draws = 0
    for line in path.read_text(errors='replace').splitlines():
        if line.startswith('F '):
            match = FRAME.match(line)
            if not match:
                continue
            values = [int(v) for v in match.groups()[1:]] + [draws]
            for field, value in zip(FRAME_FIELDS + ['draws'], values):
                stats['all'].setdefault(field, []).append(value)
                if value > stats['frame'].get(field, (0, ''))[0]:
                    stats['frame'][field] = (value, name)
            draws = 0
        elif line.startswith('T '):
            for item in line.split()[1:]:
                key, value = item.split(':')
                count, size = (int(v) for v in value.split('/'))
                label = 'vertex buffers' if key == 'vb' else FORMATS[int(key)]
                best = stats['live'].get(label, (0, 0, ''))
                if size > best[1]:
                    stats['live'][label] = (count, size, name)
            total = sum(int(item.split(':')[1].split('/')[1]) for item in line.split()[1:] if not item.startswith('vb'))
            if total > stats['live_total'][0]:
                stats['live_total'] = (total, name)
        else:
            match = DRAW.search(line)
            if not match:
                continue
            draws += 1
            fragment, pipeline = int(match[1], 16), int(match[2], 16)
            stats['fragments'].setdefault(fragment, name)
            stats['pipelines'].setdefault(pipeline, name)
            stats['triples'].setdefault(triple(fragment, pipeline), name)
            if match[3] is not None:
                stats['args'].setdefault((fragment, int(match[3], 16)), name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('traces', type=Path, nargs='+')
    parser.add_argument('--variants', type=Path, help='write the key sets here')
    args = parser.parse_args()
    stats = {'frame': {}, 'all': {}, 'live': {}, 'live_total': (0, ''), 'fragments': {}, 'pipelines': {}, 'triples': {},
             'args': {}}
    for path in args.traces:
        read(path, path.parent.name, stats)

    print('Per frame, max (run) and 99th percentile over all frames:')
    for field in FRAME_FIELDS + ['draws']:
        value, name = stats['frame'].get(field, (0, '-'))
        values = sorted(stats['all'].get(field, [0]))
        p99 = values[min(len(values) - 1, int(len(values) * 0.99))]
        print(f'  {field:20} {value:>10}  ({name})  p99 {p99}')
    print('Live, max bytes (count at that point, run):')
    for label in FORMATS + ['vertex buffers']:
        count, size, name = stats['live'].get(label, (0, 0, '-'))
        print(f'  {label:20} {size / 1024:>10.0f} KB  {count:>5} ({name})')
    total, name = stats['live_total']
    print(f'  {"all textures":20} {total / 1024:>10.0f} KB  ({name})')
    print(f'Distinct fragment keys: {len(stats["fragments"])}')
    print(f'Distinct pipeline keys: {len(stats["pipelines"])}')
    print(f'Distinct (fragment key, blend pair, vertex program): {len(stats["triples"])}')
    print(f'Distinct (fragment key, arguments key): {len(stats["args"])}')

    if args.variants:
        lines = ['# GXM shader variants: the keys the PC runs drew with (tools/trace_stats.py).',
                 '# fragment <key> (render/shader_key.h), triple <key> <blend src/dst or off> <tl|3d>',
                 '# and args <key> <arguments key>, each with the run that first used it.']
        lines += [f'fragment {k:09x}  # {n}' for k, n in sorted(stats['fragments'].items())]
        lines += [f'triple {t}  # {n}' for t, n in sorted(stats['triples'].items())]
        lines += [f'args {k:09x} {a:011x}  # {n}' for (k, a), n in sorted(stats['args'].items())]
        args.variants.parent.mkdir(parents=True, exist_ok=True)
        args.variants.write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
