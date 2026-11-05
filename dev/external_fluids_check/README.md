# On-demand fluid data

Before the first fluid library access, including `FluidsList`, install a
synchronous C++ provider through `CoolProp/CoolProp.h`:

```cpp
CoolProp::set_fluid_data_provider([](const std::string& file) -> std::string {
    // Return the raw bytes of dev/fluids_cbor/<file> from host storage.
    return read_binary_file(file);
});
```

An empty function selects the usual embedded library. Replacing or clearing the
provider after initialization begins throws `ValueError`. When
`COOLPROP_EXTERNAL_FLUIDS=ON`, the compiled library contains only the index;
access without a provider throws `ValueError`. Installing a provider after that
missing-provider error permits another initialization attempt.

The provider runs while the library mutex is held. It may query `FluidsList` or
other fluids, but must not request the same fluid recursively or wait for another
thread that needs the fluid library. Successful requests are cached. Exceptions,
invalid CBOR, invalid fluid parsing, or a mismatched fluid name raise `ValueError`
with the requested fluid name; a subsequent request retries. This does not make
concurrent use of the same AbstractState or global configuration changes safe.

JavaScript hosts call `set_fluid_data_provider_js(file => cachedBytes[file])`.
The callback must return a `Uint8Array` synchronously; a Promise, null, another
type, or a thrown exception causes a retryable error. Passing null or undefined
clears the provider before initialization. Hosts preload the fluids they need
into their cache before synchronous CoolProp calls.

## Generated format

`dev/generate_headers.py` continues to generate the combined CBOR and existing
headers. It additionally generates the ignored build outputs:

- `dev/fluids_cbor/fluid-<hex>.cbor`: one fluid **object**, without an enclosing
  array, byte-identical to that element in `all_fluids.cbor`. `<hex>` is lowercase
  hexadecimal of the canonical name's UTF-8 bytes. This is reversible and safe
  for paths, URLs, punctuation, and case-insensitive filesystems.
- `dev/fluids_cbor/index.cbor`: an array ordered exactly like `all_fluids.cbor`
  (sorted source file paths). Every entry has `NAME`, `CAS`, `REFPROP_NAME`,
  `ALIASES`, and `file`. For this checkout it is 15,902 bytes and has 136 entries.
- `include/fluid_index_CBOR.h`: the same index bytes as a constexpr byte array.

Deploy matching per-fluid assets with the binary. The provider receives only
`file`, without a directory prefix. A changed EOS requires redeploying assets
with the new binary. Generated files are restored if missing and are not rewritten
when their bytes are unchanged.

Identifier registration is shared with embedded loading: canonical name, CAS,
aliases, and uppercase aliases. Existing valid REFPROP names are included among
these aliases. `N/A` REFPROP placeholders are not added as lookup keys.

## Enumeration and access paths

`get_fluid_list()` and the high-level `FluidsList` variants enumerate names from
the index without fetching any payload. String/index `get`, JSON export (including
`get_fluid_as_JSONstring`), cubic suffix lookup, and enthalpy/entropy offset
mutation load the requested pending entry first. Mixture backends use these same
accessors to load each component. Transport models using other reference fluids
load those references through the existing backend accessors as needed.

There is no production accessor that iterates a raw Helmholtz fluid map.
Enumeration tests in `CoolProp-Tests.cpp`, `Ancillaries.cpp`, and
`REFPROPMixtureBackend.cpp` obtain the name list and then construct states or
export JSON for each fluid, so they progressively load all requested fluids.
The tests for raw embedded-CBOR equivalence and raw superancillary source hashes
are excluded in external-only builds because that blob is intentionally absent.
The standalone JSON comparison below verifies every fluid's exported data across
embedded and external modes.

## Offline native verification

From the checkout root, with dependencies already available locally:

```sh
./configure-native.sh build-native -DFETCHCONTENT_FULLY_DISCONNECTED=ON
~/.local/bin/cmake --build build-native --target CoolProp -j 4
python3 dev/external_fluids_check/run.py
node dev/external_fluids_check/check_js_helper.js
```

`run.py` builds a standalone executable against `build-native/libCoolProp.a`,
using that build's compiler flags and dependency paths. Each fluid grid runs in
separate embedded and external processes. It compares exact `%.17g` output and
exception text for nine trivial outputs, six states, fourteen outputs per state,
surface tension, and enthalpy/pressure, pressure/entropy, and density/temperature
flashes. It also compares complete JSON exports and every registered identifier,
mixtures, aliases, PropsSI errors, cubic suffixes, reference offsets, additions
and overwrites. Cold concurrent loading uses 16 threads. Additional tests cover
provider setup restrictions, retry after provider/decode/parse failure, recursive
provider requests, and zero payload fetches for `FluidsList`.

To verify the external-only build against the saved embedded results:

```sh
./configure-native.sh build-native-external -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DCOOLPROP_EXTERNAL_FLUIDS=ON
~/.local/bin/cmake --build build-native-external --target CoolProp -j 4
python3 dev/external_fluids_check/run.py --build build-native-external --external-only
```

The script also verifies that no `gall_fluids_CBOR` symbols remain and checks
missing-provider errors and recovery. Results are retained under ignored
`dev/external_fluids_check/results/`. RSS is measured with `getrusage` in fresh
native child processes that load Water and call PropsSI once. Forking before
loading avoids inheriting the Python comparison runner's peak RSS. The Node helper check is not an
Emscripten compile or an end-to-end JavaScript binding test.

## Verified results

Both RelWithDebInfo static-library builds passed with
`LAZY_LOAD_SUPERANCILLARIES` and four build jobs. The default binary's embedded
and external processes matched for all 136 fluids: **16,565 numeric output
records and 1,521 error records**, including the exact error messages. The
1,521 records are expected grid errors (unsupported transport properties,
invalid states, or unavailable flash paths), not failures of the comparison.
Complete JSON exports (9,641,606 bytes) and 966 identifier lookups also matched.
The external-only binary matched the same baseline, and `nm` confirmed that it
contains no combined-fluid CBOR symbols.

Mixtures, aliases, PropsSI, cubic suffixes, pending reference offsets,
addition/overwrite, provider lifecycle, ten provider/decode/parse failures with
successful retry, recursive loading rejection, and 16-thread cold loading passed.
The generator restored missing assets and verified every individual CBOR object
against the combined payload. The Node helper checks passed.

| Binary and loading mode | Peak RSS, KiB |
| --- | ---: |
| Default binary, embedded | 56,820 |
| Default binary, external provider | 7,196 |
| External-only binary, external provider | 7,028 |

These are native Linux `getrusage` measurements from the clean child processes
specified above; they vary somewhat between runs. They do not measure
WebAssembly memory. Emscripten/embind compilation and an end-to-end JavaScript
module test were not performed, as requested. The full Catch suite was not run;
the standalone checker provides the reported coverage.
