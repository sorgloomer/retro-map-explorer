// The layers of the loaded level (markers and lines of one kind), in groups that can be switched as a whole.
import { useEffect, useRef } from 'preact/hooks';
import { toggleLayer, toggleLayerGroup } from './controller.ts';
import { Section } from './Section.tsx';
import { useUi } from './store.ts';
import type { LayerGroup } from './store.ts';

function GroupHead({ group, index }: { group: LayerGroup; index: number }) {
  const box = useRef<HTMLInputElement>(null);
  const used = group.rows.filter((r) => r.count > 0);
  const on = used.filter((r) => r.visible).length;
  const checked = used.length > 0 && on === used.length;
  const mixed = on > 0 && on < used.length;
  useEffect(() => { if (box.current) box.current.indeterminate = mixed; });
  return (
    <label class="lgroup">
      <input ref={box} type="checkbox" checked={checked} onChange={(e) => { toggleLayerGroup(index, e.currentTarget.checked); e.currentTarget.blur(); }} />
      <span>{group.title}</span>
    </label>
  );
}

export function LayersSection() {
  const scene = useUi((s) => s.scene);
  return (
    <Section id="secLayers" title="Layers" hidden={!scene}>
      {scene && (
        <>
          <p class="levelname">{scene.title}</p>
          <p class="muted">{scene.info}</p>
          <div id="legend">
            {scene.groups.map((group, gi) => (
              <>
                <GroupHead group={group} index={gi} />
                {group.rows.map((row, ri) => (
                  <label class={'item' + (row.count ? '' : ' empty')}>
                    <input type="checkbox" checked={row.visible} onChange={(e) => { toggleLayer(gi, ri, e.currentTarget.checked); e.currentTarget.blur(); }} />
                    <span class="sw" style={{ background: row.color }} />
                    <span class="name">{row.name}</span>
                    <span class="count">{row.count}</span>
                  </label>
                ))}
              </>
            ))}
          </div>
        </>
      )}
    </Section>
  );
}
