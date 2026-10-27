// The overlay of the driving minigame: back button, score, time, speed and the end-of-round message.
import { stopGame } from './controller.ts';
import { useUi } from './store.ts';

export function GameHud() {
  const on = useUi((s) => s.gameMode);
  const hud = useUi((s) => s.hud);
  return (
    <div id="gameHud" hidden={!on}>
      <button type="button" title="Back to the viewer (Esc)" onClick={stopGame}>← Back</button>
      <div class="gh-stats">
        <div><span>Score</span><b>{hud.score}</b></div>
        <div><span>Time</span><b style={hud.urgent ? { color: '#ff6b6b' } : undefined}>{hud.time}</b></div>
        <div><span>Powerups</span><b>{hud.pups}</b></div>
        <div><span>Drift</span><b>{hud.drift}</b></div>
      </div>
      <div id="ghMsg" style={{ opacity: hud.messageVisible ? '1' : '0' }}>{hud.message}</div>
      <div class="gh-speed"><b>{hud.speed}</b><span>km/h</span></div>
      <div id="ghOver" hidden={!hud.over}>
        <h2>{hud.overTitle}</h2>
        <p>{hud.overText}</p>
        <p class="muted"><kbd>Enter</kbd> new round &nbsp; <kbd>Esc</kbd> back to the viewer</p>
      </div>
      <p class="gh-help">
        <kbd>W</kbd><kbd>S</kbd> gas / brake · <kbd>A</kbd><kbd>D</kbd> steer · <kbd>Space</kbd> handbrake · <kbd>R</kbd> upright ·
        <kbd>C</kbd> camera · <kbd>Esc</kbd> exit — or use a gamepad (RT / LT, left stick, A, Y, Back)
      </p>
    </div>
  );
}
