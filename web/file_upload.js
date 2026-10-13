// SPDX-License-Identifier: GPL-2.0-only
// Passes folders and files from the browser to the Emscripten editor.
//
// The desktop editor is given a folder. Browsers can't hand over a real folder
// path, so this copies the chosen files into a new virtual folder and passes
// that folder to pafta_web_files_ready(). Using a new folder each time means a
// second map opened in the same session never picks up files from the first.
(function installPaftaFileUpload(global) {
  let uploadNumber = 0;
  let requestNumber = 0;
  const uploadRoots = new Set();

  function releaseProject(root) {
    if (!uploadRoots.has(root)) return;
    function removeDirectory(directory) {
      for (const name of Module.FS.readdir(directory)) {
        if (name === '.' || name === '..') continue;
        const path = `${directory}/${name}`;
        if (Module.FS.isDir(Module.FS.stat(path).mode)) removeDirectory(path);
        else Module.FS.unlink(path);
      }
      Module.FS.rmdir(directory);
    }
    removeDirectory(root);
    uploadRoots.delete(root);
  }

  function setStatus(text) {
    const node = document.getElementById('status');
    if (node) node.textContent = text;
  }

  function cleanRelativePath(file) {
    const raw = file.webkitRelativePath || file.relativePath || file.name;
    const parts = raw.split('/').filter((part) => part && part !== '.' && part !== '..');
    // webkitdirectory puts the chosen folder's name at the start of each path.
    if (parts.length > 1) parts.shift();
    return parts.join('/');
  }

  async function upload(fileList, request = ++requestNumber) {
    const selectedFiles = Array.from(fileList || []).filter((file) => file && file.size >= 0);
    // city.bin is a cache the game makes while running, and can be hundreds of
    // MiB. The editor reads city2.sbc and the CSX/SBB/SBL files, so don't copy
    // this unused cache into the browser's file system when uploading a folder.
    const files = selectedFiles.filter((file) => cleanRelativePath(file).toLowerCase() !== 'city.bin');
    if (!files.length) return;

    if (request !== requestNumber) return;
    const root = `/web_project_${++uploadNumber}`;
    const skippedCache = selectedFiles.length - files.length;
    const suffix = skippedCache > 0 ? ` (${skippedCache} runtime cache skipped)` : '';
    let delivered = false;
    try {
      Module.FS.mkdirTree(root);
      uploadRoots.add(root);
      setStatus(`Copying ${files.length} files into the editor...${suffix}`);
      let copied = 0;
      for (const file of files) {
        const bytes = new Uint8Array(await file.arrayBuffer());
        if (request !== requestNumber) return;
        const relative = cleanRelativePath(file);
        if (!relative) continue;
        const path = `${root}/${relative}`;
        Module.FS.mkdirTree(path.slice(0, path.lastIndexOf('/')));
        Module.FS.writeFile(path, bytes);
        copied += 1;
        if (copied === files.length || copied % 16 === 0) {
          setStatus(`Copying files into the editor... ${copied}/${files.length}${suffix}`);
        }
      }
      if (request !== requestNumber) return;
      Module.ccall('pafta_web_files_ready', null, ['string'], [root]);
      delivered = true;
      setStatus('Map files copied. Loading map...');
    } catch (error) {
      if (request === requestNumber) throw error;
    } finally {
      if (!delivered) releaseProject(root);
    }
  }

  function reportUploadFailure(error) {
    console.error(error);
    setStatus('Map upload failed: ' + error.message);
  }

  async function collectEntryFiles(entry, parent = '') {
    const path = parent + entry.name;
    if (entry.isFile) {
      const file = await new Promise((resolve, reject) => entry.file(resolve, reject));
      return [{name: file.name, size: file.size, relativePath: path, arrayBuffer: () => file.arrayBuffer()}];
    }
    if (!entry.isDirectory) return [];
    const reader = entry.createReader();
    const files = [];
    // A folder reader may only return one batch (often 100 entries) at a time.
    while (true) {
      const entries = await new Promise((resolve, reject) => reader.readEntries(resolve, reject));
      if (!entries.length) return files;
      for (const child of entries) files.push(...await collectEntryFiles(child, path + '/'));
    }
  }

  async function uploadDrop(dataTransfer) {
    const request = ++requestNumber;
    const entries = Array.from(dataTransfer.items || [])
      .map((item) => typeof item.webkitGetAsEntry === 'function' ? item.webkitGetAsEntry() : null)
      .filter(Boolean);
    if (entries.some((entry) => entry.isDirectory)) {
      const files = [];
      for (const entry of entries) files.push(...await collectEntryFiles(entry));
      await upload(files, request);
    } else {
      await upload(dataTransfer.files, request);
    }
  }

  function openPicker() {
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.webkitdirectory = true;
    input.addEventListener('change', () => { void upload(input.files).catch(reportUploadFailure); });
    input.click();
  }

  function installDropTarget(target) {
    target.addEventListener('dragover', (event) => {
      event.preventDefault();
      event.dataTransfer.dropEffect = 'copy';
    });
    target.addEventListener('drop', (event) => {
      event.preventDefault();
      void uploadDrop(event.dataTransfer).catch(reportUploadFailure);
    });
  }

  global.paftaBrowserOpenPicker = openPicker;
  global.paftaBrowserInstallDropTarget = installDropTarget;
  global.paftaBrowserUpload = upload;
  global.paftaBrowserReleaseProject = releaseProject;
}(window));
