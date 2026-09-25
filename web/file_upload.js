// Browser-side directory/file bridge for the Emscripten editor.
//
// The native editor receives a folder. Browsers cannot expose a real folder
// path, so this copies the selected files into a fresh virtual directory and
// passes that directory to map_editor_web_files_ready(). A fresh root avoids
// stale assets when the user opens a second map in the same session.
(function installMapEditorFileUpload(global) {
  let uploadNumber = 0;

  function setStatus(text) {
    const node = document.getElementById('status');
    if (node) node.textContent = text;
  }

  function cleanRelativePath(file) {
    const raw = file.webkitRelativePath || file.name;
    const parts = raw.split('/').filter((part) => part && part !== '.' && part !== '..');
    // webkitdirectory includes the selected directory as its first component.
    if (parts.length > 1) parts.shift();
    return parts.join('/');
  }

  async function upload(fileList) {
    const selectedFiles = Array.from(fileList || []).filter((file) => file && file.size >= 0);
    // city.bin is a game-side runtime cache and can be hundreds of MiB. The
    // editor reads city2.sbc plus CSX/SBB/SBL assets, so do not copy this unused
    // cache into the browser filesystem during folder upload.
    const files = selectedFiles.filter((file) => cleanRelativePath(file).toLowerCase() !== 'city.bin');
    if (!files.length) return;

    const root = `/web_project_${++uploadNumber}`;
    const skippedCache = selectedFiles.length - files.length;
    const suffix = skippedCache > 0 ? ` (${skippedCache} runtime cache skipped)` : '';
    setStatus(`Copying ${files.length} files into the editor...${suffix}`);
    Module.FS.mkdirTree(root);
    let copied = 0;
    await Promise.all(files.map(async (file) => {
      const relative = cleanRelativePath(file);
      if (!relative) return;
      const path = `${root}/${relative}`;
      const slash = path.lastIndexOf('/');
      Module.FS.mkdirTree(path.slice(0, slash));
      Module.FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
      copied += 1;
      if (copied === files.length || copied % 16 === 0) {
        setStatus(`Copying files into the editor... ${copied}/${files.length}${suffix}`);
      }
    }));

    Module.mapEditorLastUploadRoot = root;
    if (typeof Module.ccall === 'function') {
      Module.ccall('map_editor_web_files_ready', null, ['string'], [root]);
    } else if (typeof Module._map_editor_web_files_ready === 'function') {
      Module._map_editor_web_files_ready(root);
    }
    setStatus('Map files copied. Loading map...');
  }

  function openPicker() {
    const input = document.createElement('input');
    input.type = 'file';
    input.multiple = true;
    input.webkitdirectory = true;
    input.addEventListener('change', () => { void upload(input.files); });
    input.click();
  }

  function installDropTarget(target) {
    target.addEventListener('dragover', (event) => {
      event.preventDefault();
      event.dataTransfer.dropEffect = 'copy';
    });
    target.addEventListener('drop', (event) => {
      event.preventDefault();
      void upload(event.dataTransfer.files);
    });
  }

  global.mapEditorBrowserOpenPicker = openPicker;
  global.mapEditorBrowserInstallDropTarget = installDropTarget;
  global.mapEditorBrowserUpload = upload;
}(window));
