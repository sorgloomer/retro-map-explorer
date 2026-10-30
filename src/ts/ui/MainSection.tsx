// The first section: share link, game and level, the minigame, opening game files and the camera.
import { cameraCommand, chooseCar, chooseGame, chooseLevel, chooseZip, openFolder, setCameraMode, shareView, startGame } from './controller.ts';
import { Section } from './Section.tsx';
import { useUi } from './store.ts';

function Selectors() {
  const games = useUi((s) => s.games);
  const gamesMessage = useUi((s) => s.gamesMessage);
  const gameId = useUi((s) => s.gameId);
  const groups = useUi((s) => s.levelGroups);
  const levelId = useUi((s) => s.levelId);
  const levels = (list: { id: string; name: string }[]) => list.map((l) => <option value={l.id}>{l.name}</option>);
  return (
    <>
      <label>Game
        <select disabled={!games.length} value={gameId} onChange={(e) => { chooseGame(e.currentTarget.value); e.currentTarget.blur(); }}>
          {games.length ? games.map((g) => <option value={g.id}>{g.label}</option>) : <option value="">{gamesMessage}</option>}
        </select>
      </label>
      <label>Level
        <select disabled={!groups.length} value={levelId} onChange={(e) => { chooseLevel(e.currentTarget.value); e.currentTarget.blur(); }}>
          {groups.length ? groups.map((g) => (g.title === null ? levels(g.levels) : <optgroup label={g.title}>{levels(g.levels)}</optgroup>)) : <option value="">—</option>}
        </select>
      </label>
    </>
  );
}

function CameraControls() {
  const mode = useUi((s) => s.camMode);
  const touch = useUi((s) => s.touch);
  return (
    <>
      <h3>Camera</h3>
      <div class="row">
        <button type="button" onClick={() => cameraCommand(0)}>Overview</button>
        <button type="button" onClick={() => cameraCommand(2)}>Top-down</button>
        <button type="button" onClick={() => cameraCommand(1)}>Start</button>
      </div>
      <div class="seg" role="radiogroup" aria-label="Camera mode">
        <button type="button" class={mode === 1 ? 'on' : ''} role="radio" aria-checked={mode === 1} onClick={() => setCameraMode(1)}>Orbital</button>
        <button type="button" class={mode === 0 ? 'on' : ''} role="radio" aria-checked={mode === 0} onClick={() => setCameraMode(0)}>Flying</button>
      </div>
      <ul class="help kbd" hidden={mode !== 0}>
        <li><kbd>W</kbd><kbd>A</kbd><kbd>S</kbd><kbd>D</kbd> fly (W goes where you look)</li>
        <li><kbd>E</kbd>/<kbd>Space</kbd> up &nbsp; <kbd>Q</kbd>/<kbd>C</kbd> down</li>
        <li>Drag mouse to look around · wheel: dolly</li>
      </ul>
      <ul class="help kbd" hidden={mode !== 1}>
        <li>Left-drag: slide the map under the cursor (height stays)</li>
        <li>Right-drag: orbit around the point you press on</li>
        <li><kbd>W</kbd><kbd>A</kbd><kbd>S</kbd><kbd>D</kbd> move &nbsp; <kbd>E</kbd>/<kbd>Q</kbd> up/down</li>
        <li>Wheel: zoom towards the pivot</li>
      </ul>
      <ul class="help" hidden={!touch}>
        <li><b>Touch:</b> one finger drags the map</li>
        <li>Pinch to zoom · two fingers up/down to tilt</li>
        <li>Twist two fingers to rotate around the touched point</li>
        <li>Tap a marker for its label</li>
      </ul>
      <ul class="help kbd">
        <li><kbd>Shift</kbd> fast &nbsp; <kbd>Alt</kbd> slow &nbsp; <kbd>V</kbd> switch mode</li>
        <li><kbd>R</kbd> overview &nbsp; <kbd>T</kbd> start position</li>
      </ul>
    </>
  );
}

export function MainSection() {
  const share = useUi((s) => s.share);
  const status = useUi((s) => s.status);
  const canPlay = useUi((s) => !!s.scene?.canPlay);
  const cars = useUi((s) => s.scene?.cars ?? 0);
  const car = useUi((s) => s.car);
  const chosen = cars & (1 << car) ? car : cars & 1 ? 0 : 1;  // the other one when the chosen car's game is not open
  return (
    <Section id="secMain" title="Retro Map Explorer">
      <button type="button" title="Copy a link to this exact view" onClick={() => { void shareView(); }}>Share this view</button>
      <input id="shareUrl" type="text" readOnly hidden={!share.url} value={share.url} aria-label="Shareable link" onFocus={(e) => e.currentTarget.select()} />
      <p class="muted">{share.msg}</p>
      <Selectors />
      {canPlay && (
        <label title="Open both games' folders to have both cars. The cars keep their real size: the Eagle is a big car on a Re-Volt table, the RC car is smaller than a pedestrian in Carmageddon.">Car
          <select value={chosen} onChange={(e) => { chooseCar(Number(e.currentTarget.value)); e.currentTarget.blur(); }}>
            <optgroup label="Carmageddon TDR 2000">
              <option value="0" disabled={!(cars & 1)}>Eagle</option>
            </optgroup>
            <optgroup label="Re-Volt">
              <option value="1" disabled={!(cars & 2)}>RC Bandit</option>
            </optgroup>
          </select>
        </label>
      )}
      <div class="row" hidden={!canPlay}>
        <button class="play" type="button" title="Drive the car through this level and collect the powerups before time runs out" onClick={() => { void startGame(0); }}>▶ Powerup Rush</button>
        <button class="play alt" type="button" title="Drive the car around this level with no time limit" onClick={() => { void startGame(1); }}>Free drive</button>
      </div>
      <div class="row">
        <button type="button" onClick={() => { void openFolder(); }}>Open game folder…</button>
        <button type="button" onClick={chooseZip}>Open .zip…</button>
      </div>
      <p class="muted" style={status.error ? { color: '#ff8a8a' } : undefined}>{status.text}</p>
      <CameraControls />
    </Section>
  );
}
