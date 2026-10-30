// The overlay of the driving minigame: back button, score, time, speed and the end-of-round message.
import type { ComponentChildren } from 'preact';
import { useEffect, useRef, useState } from 'preact/hooks';
import { TOUCH, gameParamsJson, gameStick, gameTouch, saveGameParams, setDebugForces, setDebugSpheres, setParamsEditor, stopGame, toggleDebugPanel } from './controller.ts';
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

/** A small (i) that shows its text in a popup on hover, focus or touch. */
function Info({ children }: { children: ComponentChildren }) {
  return (
    <span class="gd-info" tabIndex={0} role="note" aria-label="Info" onClick={(e) => e.preventDefault()}>
      ⓘ<span class="gd-pop">{children}</span>
    </span>
  );
}

/** One number of the table: a fixed width, aligned to the right, so the digits of the rows line up. */
function Num({ v, digits = 1, unit = '' }: { v: number; digits?: number; unit?: string }) {
  return <span class="gd-n">{v.toFixed(digits)}{unit}</span>;
}

const WHEEL_NAMES = ['FL', 'FR', 'RL', 'RR'];

/** Debug panel: the sideways force of each tyre against its limit. A full bar is the grip limit; red: the tyre slides. */
function LateralForces() {
  const wheels = useUi((s) => s.wheels);
  const shown = useUi((s) => s.debugPanel);
  const forces = useUi((s) => s.debugForces);
  const torque = useUi((s) => s.torque);
  // the forces are in kN, and in N for the toy car (the RC Bandit: its forces are a few newtons)
  const div = torque.toy ? 1 : 1000, unit = torque.toy ? 'N' : 'kN', torqueDigits = torque.toy ? 2 : 1;
  const spheres = useUi((s) => s.debugSpheres);
  const playing = useUi((s) => s.gameMode);
  const total = wheels.reduce((sum, w) => sum + w.lat, 0);
  // L shows and hides the panel (the game does not use that key)
  useEffect(() => {
    if (!playing) return;
    const onKey = (e: KeyboardEvent) => {
      if ((e.target as HTMLElement | null)?.tagName === 'TEXTAREA') return;  // typing in the parameter editor
      if (e.code === 'KeyL' && !e.repeat && !e.ctrlKey && !e.metaKey && !e.altKey) toggleDebugPanel();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [playing]);
  return (
    <div class={'gh-debug' + (shown ? '' : ' closed')}>
      <div class="gd-head">
        <button type="button" class="gd-title" onClick={(e) => { toggleDebugPanel(); e.currentTarget.blur(); }}
          title="Show or hide the lateral force panel (L)">{shown ? '▾' : '▸'} tyre forces <kbd>L</kbd></button>
        <button type="button" class="gd-config" onClick={(e) => { setParamsEditor(true); e.currentTarget.blur(); }}
          title="Edit the minigame's parameters as JSON">configure</button>
      </div>
      {shown && (
        <div class="gd-table">
          <span class="gd-h gd-name" />
          <span class="gd-h" title="T: the wheel is on the ground">grounded</span>
          <span class="gd-h">force bars</span>
          <span class="gd-h">lateral</span>
          <span class="gd-h">longitudinal</span>
          <span class="gd-h">slip</span>
          <span class="gd-h">suspension</span>
          <span class="gd-h gd-name" />
          <span class="gd-h gd-sub" title="T: the wheel is on the road">T = yes</span>
          <span class="gd-h gd-sub" title="green: sideways force, amber: force along the wheel; a full half bar is the grip limit">side / along</span>
          <span class="gd-h gd-sub" title={'sideways force / its limit, in ' + unit + '; + is towards the right'}>{unit} / limit</span>
          <span class="gd-h gd-sub" title={'force along the wheel / the friction limit, in ' + unit + '; + is forward'}>{unit} / limit</span>
          <span class="gd-h gd-sub" title="slip angle">deg</span>
          <span class="gd-h gd-sub" title="suspension force, the load on the tyre">{unit}</span>
          {wheels.map((w, i) => (
            <div class={'gd-row' + (w.slide ? ' slide' : '')}>
              <span class="gd-name">{WHEEL_NAMES[i]}</span>
              <span class="gd-ground">{w.g ? 'T' : '.'}</span>
              <span class="gd-bars">
                <span class="gd-bar">
                  <i style={{ left: w.lat < 0 ? `${50 - Math.min(50, (-w.lat / (w.limit || 1)) * 50)}%` : '50%', width: `${Math.min(50, (Math.abs(w.lat) / (w.limit || 1)) * 50)}%` }} />
                </span>
                <span class="gd-bar gd-long">
                  <i style={{ left: w.lon < 0 ? `${50 - Math.min(50, (-w.lon / (w.fric || 1)) * 50)}%` : '50%', width: `${Math.min(50, (Math.abs(w.lon) / (w.fric || 1)) * 50)}%` }} />
                </span>
              </span>
              <span class="gd-pair"><Num v={w.lat / div} /><i>/</i><Num v={w.limit / div} /></span>
              <span class="gd-pair"><Num v={w.lon / div} /><i>/</i><Num v={w.fric / div} /></span>
              <span class="gd-pair"><Num v={w.slip * 57.3} digits={0} unit="°" /></span>
              <span class="gd-pair gd-susp"><Num v={w.load / div} /></span>
            </div>
          ))}
        </div>
      )}
      {shown && (
        <div class="gd-sums gd-totals">
          <span class="gd-h">totals {unit}</span><span class="gd-h">lateral</span><span class="gd-h">longitudinal</span><span class="gd-h">suspension</span>
          <span class="gd-h" />
          <span class="gd-pair"><Num v={total / div} /></span>
          <span class="gd-pair"><Num v={wheels.reduce((sum, w) => sum + w.lon, 0) / div} /></span>
          <span class="gd-pair gd-susp"><Num v={wheels.reduce((sum, w) => sum + w.load, 0) / div} /></span>
        </div>
      )}
      {shown && (
        <div class="gd-sums" title="Torque about the centre of mass around the car's right (pitch), up (yaw) and forward (roll) axes. Gravity, air drag, downforce and the afterburner push at the centre of mass itself and turn nothing.">
          <span class="gd-h">hull torque {unit} m</span><span class="gd-h">pitch</span><span class="gd-h">yaw</span><span class="gd-h">roll</span>
          <span class="gd-h">from wheels</span>{torque.wheels.map((x) => <span class="gd-pair"><Num v={x} digits={torqueDigits} /></span>)}
          <span class="gd-h">from hull collisions</span>{torque.hull.map((x) => <span class="gd-pair"><Num v={x} digits={torqueDigits} /></span>)}
          <span class="gd-h">spin rad/s</span>{torque.spin.map((x) => <span class="gd-pair"><Num v={x} digits={torqueDigits} /></span>)}
        </div>
      )}
      {shown && (
        <div class="gd-checks">
          <label class="gd-forces">
            <input type="checkbox" checked={forces} onChange={(e) => setDebugForces(e.currentTarget.checked)} /> force vectors
            <Info>
              Arrows from each tyre (suspension and tyre together) and from the centre of mass. A car's weight is 4 m long.
              <span class="gd-key" style={{ color: '#ffdc3c' }}>■ wheels</span> <span class="gd-key" style={{ color: '#4678ff' }}>■ gravity</span>{' '}
              <span class="gd-key" style={{ color: '#ff4646' }}>■ air drag</span> <span class="gd-key" style={{ color: '#3ce6e6' }}>■ downforce</span>{' '}
              <span class="gd-key" style={{ color: '#ff961e' }}>■ afterburner</span> <span class="gd-key" style={{ color: '#bebebe' }}>■ hull contact</span>
            </Info>
          </label>
          <label class="gd-forces">
            <input type="checkbox" checked={spheres} onChange={(e) => setDebugSpheres(e.currentTarget.checked)} /> collision spheres
            <Info>
              The spheres the chassis collides with: <span class="gd-key" style={{ color: '#3cdc5a' }}>■ green</span> free,{' '}
              <span class="gd-key" style={{ color: '#ff3c3c' }}>■ red</span> touching the level (during the last frame).
            </Info>
          </label>
        </div>
      )}
    </div>
  );
}

/** The minigame's parameters as JSON text to edit. Save applies them at once (the car follows) and remembers them; Reset puts the
 * built-in values in the text (not saved until Save). A text that is not valid is refused with the line number. */
function ParamsEditor() {
  const open = useUi((s) => s.paramsOpen);
  const [text, setText] = useState('');
  const [message, setMessage] = useState<{ text: string; error: boolean } | null>(null);
  const field = useRef<HTMLTextAreaElement>(null);
  useEffect(() => {
    if (open) { setText(gameParamsJson()); setMessage(null); }
  }, [open]);
  if (!open) return null;
  const save = () => {
    const error = saveGameParams(field.current?.value ?? text);  // what is in the field now
    setMessage(error ? { text: error, error: true } : { text: 'Saved', error: false });
  };
  return (
    <div class="gh-params" role="dialog" aria-label="Minigame parameters">
      <div class="gp-box">
        <div class="gp-head">Minigame parameters (JSON: "name": number, the cars under "cars"; a missing name gets its default)</div>
        <textarea class="gp-text" ref={field} spellcheck={false} value={text} onInput={(e) => { setText(e.currentTarget.value); setMessage(null); }} />
        <div class="gp-foot">
          <span class={'gp-msg' + (message?.error ? ' error' : '')}>{message?.text ?? ''}</span>
          <button type="button" onClick={() => { setText(gameParamsJson(true)); setMessage(null); }}>Reset to defaults</button>
          <button type="button" class="primary" onClick={save}>Save</button>
          <button type="button" onClick={() => setParamsEditor(false)}>Close</button>
        </div>
      </div>
    </div>
  );
}

export function GameHud() {
  const on = useUi((s) => s.gameMode);
  const hud = useUi((s) => s.hud);
  return (
    <div id="gameHud" hidden={!on}>
      <LateralForces />
      <ParamsEditor />
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
        <kbd>C</kbd> camera · <kbd>1</kbd> afterburner · <kbd>2</kbd> kangaroo · <kbd>Esc</kbd> exit — or use a gamepad (RT / LT, left stick, A, Y, Back)
      </p>
    </div>
  );
}
