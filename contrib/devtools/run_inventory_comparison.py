#!/usr/bin/env python3
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

results = Path('results')
for repeat in range(1, 4):
    for variant in (['baseline', 'candidate'] if repeat % 2 else ['candidate', 'baseline']):
        name = f'native-{repeat}-{variant}'
        with (results / f'{name}.log').open('w') as log:
            subprocess.run([f'/tmp/bench-{variant}', '-filter=InventoryBatch.*', '-min-time=5000',
                            f'-output-json={results / name}.json'], stdout=log, stderr=subprocess.STDOUT, check=True)
        print(name, flush=True)

for scenario, batch, rounds, peers, inv_type in [('large', 50000, 8, 1, 6), ('normal', 100, 8, 4, 6), ('governance', 5000, 8, 4, 18)]:
    for repeat in range(1, 4):
        for variant in (['baseline', 'candidate'] if repeat % 2 else ['candidate', 'baseline']):
            name = f'{scenario}-{repeat}-{variant}'
            directory = Path('/tmp') / f'inventory-{name}'
            command = ['/tmp/inventory-venv/bin/python', 'contrib/devtools/benchmark_inventory.py',
                       '--configfile=test/config.ini', f'--tmpdir={directory}', '--nocleanup',
                       '--randomseed=6990', '--portseed=6990', f'--batch={batch}', f'--rounds={rounds}',
                       f'--peers={peers}', f'--inv-type={inv_type}']
            with (results / f'{name}.log').open('w') as log:
                subprocess.run(command, env=dict(os.environ, PYTHONPATH='test/functional', DASHD=f'/tmp/dashd-{variant}'),
                               stdout=log, stderr=subprocess.STDOUT, check=True, timeout=180)
            for filename in ('metrics.json', 'rpc-latencies-ms.json'):
                shutil.copyfile(directory / filename, results / f'{name}-{filename}')
            waits = {}
            for line in (directory / 'node0/regtest/debug.log').read_text().splitlines():
                match = re.search(r'lock contention (.+) completed \((\d+)μs\)', line)
                if match:
                    waits.setdefault(match[1], []).append(int(match[2]))
            (results / f'{name}-lock-waits.json').write_text(json.dumps(waits) + '\n')
            print(name, (directory / 'metrics.json').read_text().replace('\n', ' '), flush=True)
