"""Check generated fluid assets against the combined CBOR, using only stdlib."""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import cbor_min

root = Path(__file__).resolve().parents[2]
master_blob = (root / 'dev/all_fluids.cbor').read_bytes()
master = cbor_min.loads(master_blob)
index = cbor_min.loads((root / 'dev/fluids_cbor/index.cbor').read_bytes())
assert len(index) == len(master)
parts = []
for entry, fluid in zip(index, master):
    assert {k: entry[k] for k in ('NAME', 'CAS', 'REFPROP_NAME', 'ALIASES')} == {
        k: fluid['INFO'][k] for k in ('NAME', 'CAS', 'REFPROP_NAME', 'ALIASES')}
    assert re.fullmatch(r'fluid-[0-9a-f]+\.cbor', entry['file'])
    assert entry['file'] == 'fluid-' + fluid['INFO']['NAME'].encode('utf-8').hex() + '.cbor'
    blob = (root / 'dev/fluids_cbor' / entry['file']).read_bytes()
    assert cbor_min.loads(blob) == fluid
    assert blob == cbor_min.dumps(fluid)
    parts.append(blob)
# The combined payload is precisely the array header followed by these elements.
assert master_blob.endswith(b''.join(parts))
header = (root / 'include/fluid_index_CBOR.h').read_text()
assert bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', header)) == cbor_min.dumps(index)
print(f'Generator: {len(index)} ordered fluids, metadata and CBOR bytes identical')
