// Full-window overlays: the "drop your game folder" dialog, the loading spinner and the drag hint.
import { chooseZip, openFolder, resumeFolders } from './controller.ts';
import { useUi } from './store.ts';

export function Overlays() {
  const dropOpen = useUi((s) => s.dropOpen);
  const hint = useUi((s) => s.restoreHint);
  const resume = useUi((s) => s.resumeLabel);
  const busy = useUi((s) => s.busy);
  const dragging = useUi((s) => s.dragging);
  return (
    <>
      <div id="drop" class="overlay" hidden={!dropOpen}>
        <div class="dropbox">
          <h2>Drop your game folder here</h2>
          <p>Drag the installed game's folder onto this window. Files are read locally in your browser and never uploaded.</p>
          <ul>
            <li><b>Re-Volt</b> — the folder containing <code>levels/</code></li>
            <li><b>Carmageddon TDR 2000</b> — the folder containing <code>ASSETS/</code></li>
          </ul>
          <p class="touch-only">On a phone or tablet, use the buttons below. A .zip of the game folder also works
            (store without compression for the fastest loading).</p>
          <p class="muted" hidden={!hint}>{hint}</p>
          <button type="button" hidden={!resume} onClick={() => { void resumeFolders(); }}>{resume}</button>
          <div class="row">
            <button type="button" onClick={() => { void openFolder(); }}>Choose a folder…</button>
            <button type="button" onClick={chooseZip}>Choose a .zip…</button>
          </div>
        </div>
      </div>
      <div id="loading" class="overlay" hidden={!busy.on}><div class="spinner" /><p>{busy.text}</p></div>
      <div id="dropHint" hidden={!dragging}>Drop to load game files</div>
    </>
  );
}
