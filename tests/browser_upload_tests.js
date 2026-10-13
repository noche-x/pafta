// SPDX-License-Identifier: GPL-2.0-only
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const writes = new Map();
const directories = new Set();
const readyRoots = [];
const virtualFs = {
  mkdirTree(directory) {
    const parts = directory.split('/').filter(Boolean);
    for (let i = 1; i <= parts.length; ++i) directories.add('/' + parts.slice(0, i).join('/'));
  },
  writeFile(file, bytes) {writes.set(file, Array.from(bytes));},
  stat(file) {return {mode: directories.has(file) ? 1 : 0};},
  isDir(mode) {return mode === 1;},
  readdir(directory) {
    const prefix = directory + '/';
    return ['.', '..', ...new Set([...directories, ...writes.keys()]
      .filter(name => name.startsWith(prefix)).map(name => name.slice(prefix.length).split('/')[0]))];
  },
  unlink(file) {assert(writes.delete(file));},
  rmdir(directory) {
    assert.equal(this.readdir(directory).length, 2, 'removed a nonempty upload folder');
    assert(directories.delete(directory));
  }
};
const callbacks = {};
let readyRoot;
const status = {textContent: ''};
const context = {
  window: {}, console,
  document: {getElementById: () => status},
  Module: {
    FS: virtualFs,
    ccall(name, _result, _types, args) {assert.equal(name, 'pafta_web_files_ready'); readyRoot = args[0]; readyRoots.push(readyRoot);}
  }
};
vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../web/file_upload.js'), 'utf8'), context);
const file = (name, bytes) => ({name, size: bytes.length, arrayBuffer: async () => Uint8Array.from(bytes).buffer});
const entry = (name, bytes) => ({name, isFile: true, file(resolve) {resolve(file(name, bytes));}});
const directory = (name, batches) => ({
  name, isDirectory: true,
  createReader() {let index = 0; return {readEntries(resolve) {resolve(batches[index++] || []);}};}
});

(async () => {
  // The picker removes one top folder but keeps the asset folders we need.
  await context.window.paftaBrowserUpload([
    {...file('city2.sbc', [1]), webkitRelativePath: 'test/city2.sbc'},
    {...file('bank.sbb', [2]), webkitRelativePath: 'test/building/bank.sbb'},
    {...file('city.bin', [3]), webkitRelativePath: 'test/city.bin'}
  ]);
  assert.deepEqual(writes.get(readyRoot + '/building/bank.sbb'), [2]);
  assert(!writes.has(readyRoot + '/city.bin'));
  context.window.paftaBrowserInstallDropTarget({addEventListener(name, callback) {callbacks[name] = callback;}});
  const map = directory('test', [
    [entry('city2.sbc', [4])],
    [directory('building', [[entry('tower.sbb', [5])]]), entry('city.bin', [6])]
  ]);
  callbacks.drop({preventDefault() {}, dataTransfer: {items: [{webkitGetAsEntry: () => map}], files: []}});
  // Wait for the real callback, not a guess at how many folders get read.
  for (let i = 0; i < 100 && !writes.has('/web_project_2/building/tower.sbb'); i++) await new Promise(setImmediate);
  assert.deepEqual(writes.get('/web_project_2/building/tower.sbb'), [5]);
  assert(!writes.has('/web_project_2/city.bin'));
  assert.equal(readyRoot, '/web_project_2');
  assert.equal(status.textContent, 'Map files copied. Loading map...');
  context.window.paftaBrowserReleaseProject('/web_project_1');
  assert(!writes.has('/web_project_1/building/bank.sbb'));
  assert(writes.has('/web_project_2/building/tower.sbb'), 'release removed the active map');

  let releaseSlow;
  const slow = context.window.paftaBrowserUpload([
    file('partial.sbb', [7]),
    {name: 'city2.sbc', size: 1, arrayBuffer: () => new Promise(resolve => {releaseSlow = resolve;})}
  ]);
  while (!releaseSlow) await new Promise(setImmediate);
  await context.window.paftaBrowserUpload([file('city2.sbc', [8])]);
  const latest = readyRoot;
  const completions = readyRoots.length;
  releaseSlow(Uint8Array.from([9]).buffer);
  await slow;
  assert.equal(readyRoot, latest, 'older upload replaced the latest map');
  assert.equal(readyRoots.length, completions, 'cancelled upload announced completion');
  assert(!directories.has('/web_project_3'), 'cancelled upload retained its partial files');
  context.window.paftaBrowserReleaseProject('/web_project_2');

  await assert.rejects(context.window.paftaBrowserUpload([
    file('partial.sbb', [10]),
    {name: 'city2.sbc', size: 1, arrayBuffer: async () => {throw new Error('read failed');}}
  ]), /read failed/);
  assert(!directories.has('/web_project_5'), 'failed upload retained its partial files');
  assert(writes.has(latest + '/city2.sbc'), 'failed upload removed the current map');
  context.window.paftaBrowserReleaseProject(latest);
  context.window.paftaBrowserReleaseProject(latest); // Already released, or a preload folder.
  assert.equal(writes.size, 0, 'replaced uploads leaked virtual files');
  console.log('Picker paths, batched drops, newest upload, failure cleanup, and project release passed.');
})().catch((error) => {console.error(error); process.exitCode = 1;});
