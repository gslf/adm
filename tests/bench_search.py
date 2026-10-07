"""Reproducible warm-cache search benchmarks, not part of the regression suite."""
import argparse
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import statistics
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mib', type=int, default=128)
parser.add_argument('--repeat', type=int, default=5)
parser.add_argument('--baseline', type=Path)
args = parser.parse_args()
assert args.mib > 0 and args.repeat > 0
root = Path(__file__).resolve().parent.parent

def run_adm(binary, directory, query, regex, threads, expected):
    result = json.loads(subprocess.check_output(
        [str(binary), str(directory), query, str(regex), str(threads)], text=True))
    assert result['matches'] == expected, result
    return result['seconds']

with tempfile.TemporaryDirectory(prefix='adm-benchmark-') as temporary:
    base = Path(temporary)
    binary = base / ('bench.exe' if os.name == 'nt' else 'bench')
    flags = shlex.split(os.environ.get('CFLAGS', '-std=c11 -O2 -D_DEFAULT_SOURCE'))
    if os.name != 'nt' and '-pthread' not in flags:
        flags.append('-pthread')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + flags +
                   ['-I' + str(root / 'src'), str(root / 'tests/bench_search.c')] +
                   [str(root / 'src' / name) for name in
                    ('search_job.c', 'pattern.c', 'path.c', 'utf8.c')] +
                   ['-o', str(binary)], check=True)
    line = (b'alpha beta gamma delta epsilon zeta eta theta ' * 24)[:1023] + b'\n'
    cases = []
    for label, files, size, dense in (
            ('large-sparse', 32, args.mib * 1024 * 1024, False),
            ('single-large', 1, args.mib * 1024 * 1024, False),
            ('regex-no-prefix', 32, max(1, args.mib // 16) * 1024 * 1024, False),
            ('large-dense', 32, max(1, args.mib // 8) * 1024 * 1024, True),
            ('small-files', 4000, 4000 * 1024, False)):
        directory = base / label
        directory.mkdir()
        expected = 0
        for i in range(files):
            length = max(1024, size // files)
            rows = max(1, length // 1024)
            with (directory / f'{i}.txt').open('wb') as stream:
                if dense:
                    content = b'needle7' + line[7:]
                    stream.write(content * rows)
                    expected += rows
                else:
                    stream.write(line * (rows - 1))
                    stream.write(b'needle7' + line[7:])
                    expected += 1
        cases.append((label, directory, '[0-9]+' if label == 'regex-no-prefix' else 'needle7',
                      int(label == 'regex-no-prefix'), expected))
        if label == 'large-sparse':
            cases.append(('regex-prefix', directory, 'needle[0-9]+', 1, expected))
    print(f'{platform.platform()}; {os.cpu_count()} logical CPUs; '
          f'{args.repeat} measured warm-cache runs; corpus {args.mib} MiB')
    print(subprocess.check_output(shlex.split(os.environ.get('CC', 'cc')) +
                                  ['--version'], text=True).splitlines()[0])
    results = []
    for label, directory, query, regex, expected in cases:
        measurements = {}
        candidates = [('adm-1', binary, 1), ('adm-2', binary, 2),
                      ('adm-4', binary, 4), ('adm-8', binary, 8), ('adm-auto', binary, 0)]
        if args.baseline:
            candidates.insert(0, ('baseline', args.baseline.resolve(), 1))
        for name, executable, threads in candidates:
            run_adm(executable, directory, query, regex, threads, expected)
            measurements[name] = statistics.median(
                run_adm(executable, directory, query, regex, threads, expected)
                for _ in range(args.repeat))
        # External tools have different output work: rg counts; grep emits
        # occurrences; adm creates a navigable row/column/length index on disk.
        for name, command in (
                ('rg', ['rg', '-uuu', '--count-matches', '-j8',
                        '-e', query, str(directory)] if regex else
                       ['rg', '-uuu', '--count-matches', '-j8', '-F',
                        '-e', query, str(directory)]),
                ('grep', ['grep', '-r', '-o', '-E' if regex else '-F',
                          '-e', query, str(directory)])):
            if not shutil.which(command[0]):
                continue
            output = subprocess.check_output(command)
            if name == 'rg':
                count = sum(int(row.rsplit(b':', 1)[1]) for row in output.splitlines())
            else:
                count = len(output.splitlines())
            assert count == expected, (name, count, expected)
            times = []
            for _ in range(args.repeat):
                start = time.perf_counter()
                subprocess.run(command, stdout=subprocess.DEVNULL, check=True)
                times.append(time.perf_counter() - start)
            measurements[name] = statistics.median(times)
        record = dict(case=label, matches=expected, seconds=measurements)
        results.append(record)
        print(json.dumps(record), flush=True)
    print('External timings include process startup and differing output work; '
          'they are reference points, not equivalent engine benchmarks.')
