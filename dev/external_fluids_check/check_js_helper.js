// Exercise the exact EM_JS helper body without compiling an Emscripten module.
const fs = require('fs');
const path = require('path');
const source = fs.readFileSync(path.join(__dirname, '../../src/emscripten_interface.cxx'), 'utf8');
const body = source.slice(source.indexOf('    try {\n        const bytes'), source.indexOf('\n});\n\n// The captured'));
global.Emval = {toValue: value => value, toHandle: value => value};
const invoke = new Function('provider', 'file', body);
for (const invalid of [null, undefined, [], new ArrayBuffer(2), Promise.resolve(new Uint8Array(2))]) {
    if (!invoke(() => invalid, 'file').error) throw Error('Accepted invalid provider result');
}
for (const error of [Error('offline'), null, 'offline']) {
    if (!invoke(() => { throw error; }, 'file').error) throw Error('Host throw escaped');
}
const bytes = new Uint8Array([0, 128, 255]);
const result = invoke(file => {
    if (file !== 'file') throw Error('Wrong filename');
    return bytes.subarray(1);
}, 'file');
if (result.data.join() !== '128,255' || result.data.buffer === bytes.buffer) throw Error('Byte copy failed');
console.log('JS helper: bytes, subarray copy, invalid returns and raw throws passed (no module build)');
