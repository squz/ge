// ge web persistence pre-run (🎯T157). Prepended to the emitted JS via
// --pre-js (see tools/web-template/CMakeLists.txt).
//
// SDL_GetPrefPath on Emscripten returns /libsdl/<org>/<app>/ (see SDL's
// src/filesystem/emscripten/SDL_sysfilesystem.c), and ge roots game.db +
// tweaks.db there (DirectRenderHost ctor). MEMFS forgets everything on
// reload, so mount IDBFS over /libsdl and:
//   - hydrate it from IndexedDB before main() runs (run dependency), and
//   - write dirty state back periodically and on tab hide/close.

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  FS.mkdir('/libsdl');
  FS.mount(IDBFS, {}, '/libsdl');
  addRunDependency('ge-idbfs-restore');
  FS.syncfs(true, function (err) {
    if (err) console.warn('ge: IDBFS restore failed:', err);
    removeRunDependency('ge-idbfs-restore');
  });
});

Module['postRun'] = Module['postRun'] || [];
Module['postRun'].push(function () {
  var syncing = false;
  function persist() {
    if (syncing) return;
    syncing = true;
    FS.syncfs(false, function (err) {
      syncing = false;
      if (err) console.warn('ge: IDBFS persist failed:', err);
    });
  }
  // Periodic write-back bounds loss to a few seconds; the visibility hook
  // covers tab switch / close (pagehide is the last reliable event on
  // mobile Safari, where unload never fires).
  setInterval(persist, 5000);
  document.addEventListener('visibilitychange', function () {
    if (document.visibilityState === 'hidden') persist();
  });
  window.addEventListener('pagehide', persist);
});
