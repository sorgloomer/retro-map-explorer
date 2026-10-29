// Display options: how the level and its powerups are drawn, how fast the camera and the animations move.
import { restartAnimation, togglePause } from './controller.ts';
import { Check, Slider } from './controls.tsx';
import { Group, Section } from './Section.tsx';
import { useUi } from './store.ts';

function AnimationLine({ enabled }: { enabled: boolean }) {
  const paused = useUi((s) => s.animPaused);
  const speed = useUi((s) => s.opts.animSpeed);
  const time = useUi((s) => s.animTime);
  const stopped = paused || speed === 0;
  const label = paused ? 'Play animation' : 'Pause animation';
  return (
    <div class={'animline' + (paused ? ' paused' : '')} id="animLine" style={enabled ? undefined : { opacity: '.5' }}>
      <span>×{speed.toFixed(2)}{stopped ? ' (stopped)' : ''} · t = {time.toFixed(1)} s</span>
      <span class="animbtns">
        <button type="button" title={label} aria-label={label} disabled={!enabled} onClick={(e) => { togglePause(); e.currentTarget.blur(); }}>
          <svg class="ico-pause" viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path fill="currentColor" d="M3 2h3.5v12H3zM9.5 2H13v12H9.5z" /></svg>
          <svg class="ico-play" viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path fill="currentColor" d="M4 2l10 6-10 6z" /></svg>
        </button>
        <button type="button" title="Restart animation from t = 0" aria-label="Restart animation" disabled={!enabled} onClick={(e) => { restartAnimation(); e.currentTarget.blur(); }}>
          <svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true"><path fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" d="M3 8a5 5 0 1 0 1.8-3.85M3 2.2v3.2h3.2" /></svg>
        </button>
      </span>
    </div>
  );
}

export function DisplaySection() {
  const scene = useUi((s) => s.scene);
  const hasCollision = !!scene?.hasCollision, canCull = !!scene?.canCull, hasMeshes = !!scene?.hasPowerupMeshes, hasMovers = !!scene?.hasMovers;
  return (
    <Section id="secDisplay" title="Display" hidden={!scene}>
      <Group id="grpMisc" title="Misc">
        <Slider optKey="optExposure" label="Brightness" min={0.2} max={5} step={0.05} />
        <Slider optKey="optSpeed" label="Camera speed" min={0.1} max={4} step={0.05} />
      </Group>
      <Group id="grpPowerups" title="Powerups">
        <Slider optKey="optPupScale" label="Powerup size" min={0} max={1} step={0.02} />
        <Check optKey="beams">Highlight beams above powerups</Check>
        <Check optKey="pupMeshes" disabled={!hasMeshes} title={hasMeshes ? '' : 'No in-game powerup meshes for this level'}>In-game powerup meshes</Check>
        <Slider optKey="optPupExposure" label="Powerup halo brightness" min={0.2} max={3} step={0.05} />
        <Slider optKey="optXray" label="See-through strength" min={0} max={1} step={0.05} />
      </Group>
      <Group id="grpRendering" title="Rendering">
        <Check optKey="optTex">Textures</Check>
        <Check optKey="optGeo">Level geometry</Check>
        <Slider optKey="optSmooth" label="Smooth meshes (passes)" min={0} max={3} step={1} showValue
          title="Subdivision passes for the level's and the collision meshes (hard edges are kept; each pass has four times the triangles). The level loads again." />
        <Check optKey="optCol" disabled={!hasCollision} title={hasCollision ? 'Translucent overlay, coloured by surface type' : 'No collision mesh for this level'}>Collision mesh (game's own)</Check>
        <Check optKey="optWire">Wireframe (level geometry)</Check>
        <Check optKey="optBack" disabled={!canCull} title={canCull ? '' : 'This game draws every face double sided'}>Render back faces</Check>
        <Slider optKey="optLine" label="Line width" min={1} max={8} step={0.5} />
      </Group>
      <Group id="grpAnimation" title="Animation">
        <Slider optKey="animSpeed" label="Animation speed" min={0} max={2} step={0.01} disabled={!hasMovers} />
        <AnimationLine enabled={hasMovers} />
      </Group>
    </Section>
  );
}
