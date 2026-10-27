// The overlay of the driving minigame: back button, score, time, speed and the end-of-round message.
import type { ComponentChildren } from 'preact';
import { useRef } from 'preact/hooks';
import { TOUCH, gameStick, gameTouch, stopGame } from './controller.ts';
import { useUi } from './store.ts';

/** An on-screen button: held while a finger is on it (several can be held at once), or a single press when `tap` is set. */
function TouchButton({ control, tap, class: cls, label, children }: { control: number; tap?: boolean; class?: string; label: string; children: ComponentChildren }) {
  const press = (e: PointerEvent) => {
    e.preventDefault();
    try { (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId); } catch { /* the pointer is already gone */ }
    (e.currentTarget as HTMLElement).classList.add('down');
    gameTouch(control, true);
    if (tap) gameTouch(control, false);
  };
  const release = (e: PointerEvent) => {
    (e.currentTarget as HTMLElement).classList.remove('down');
    if (!tap) gameTouch(control, false);
  };
  return (
    <button type="button" class={'tbtn ' + (cls || '')} aria-label={label} onPointerDown={press} onPointerUp={release} onPointerCancel={release}
      onLostPointerCapture={release} onContextMenu={(e) => e.preventDefault()}>
      {children}
    </button>
  );
}

/** The square stick: the finger's position in it steers (left / right) and sets the throttle (up gas, down brake and reverse). */
function TouchPad() {
  const knob = useRef<HTMLDivElement>(null);
  const aim = (e: PointerEvent) => {
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
    const x = Math.max(-1, Math.min(1, ((e.clientX - r.left) / r.width) * 2 - 1));
    const y = Math.max(-1, Math.min(1, 1 - ((e.clientY - r.top) / r.height) * 2));
    if (knob.current) knob.current.style.transform = `translate(${x * r.width * 0.4}px, ${-y * r.height * 0.4}px)`;
    gameStick(x, y);
  };
  const press = (e: PointerEvent) => {
    e.preventDefault();
    try { (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId); } catch { /* the pointer is already gone */ }
    (e.currentTarget as HTMLElement).classList.add('down');
    aim(e);
  };
  const move = (e: PointerEvent) => { if ((e.currentTarget as HTMLElement).classList.contains('down')) aim(e); };
  const release = (e: PointerEvent) => {
    (e.currentTarget as HTMLElement).classList.remove('down');
    if (knob.current) knob.current.style.transform = '';
    gameStick(0, 0);
  };
  return (
    <div class="tpad" role="application" aria-label="Steering and throttle stick" onPointerDown={press} onPointerMove={move} onPointerUp={release}
      onPointerCancel={release} onLostPointerCapture={release} onContextMenu={(e) => e.preventDefault()}>
      <span class="tp-up">▲</span><span class="tp-down">▼</span><span class="tp-left">◀</span><span class="tp-right">▶</span>
      <div class="tp-knob" ref={knob} />
    </div>
  );
}

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
        <p class="muted gh-keys"><kbd>Enter</kbd> new round &nbsp; <kbd>Esc</kbd> back to the viewer</p>
        <p class="gh-overbtns">
          <button type="button" onClick={() => gameTouch(TOUCH.restart, true)}>New round</button>
          <button type="button" onClick={stopGame}>Back</button>
        </p>
      </div>
      <div class="gh-touch" hidden={hud.over}>
        <div class="gt-left">
          <TouchButton control={TOUCH.handbrake} class="big" label="Handbrake">Brake</TouchButton>
        </div>
        <div class="gt-mid">
          <TouchButton control={TOUCH.camera} tap class="small" label="Camera">Cam</TouchButton>
          <TouchButton control={TOUCH.reset} tap class="small" label="Put the car upright">Upright</TouchButton>
        </div>
        <div class="gt-right">
          <TouchPad />
        </div>
      </div>
      <p class="gh-help">
        <kbd>W</kbd><kbd>S</kbd> gas / brake · <kbd>A</kbd><kbd>D</kbd> steer · <kbd>Space</kbd> handbrake · <kbd>R</kbd> upright ·
        <kbd>C</kbd> camera · <kbd>Esc</kbd> exit — or use a gamepad (RT / LT, left stick, A, Y, Back)
      </p>
    </div>
  );
}
