// The menu in the corner: a bar with the hide button and the sections.
import { toggleDock } from './controller.ts';
import { DisplaySection } from './DisplaySection.tsx';
import { LayersSection } from './LayersSection.tsx';
import { MainSection } from './MainSection.tsx';
import { useUi } from './store.ts';

export function Dock() {
  const min = useUi((s) => s.dockMin);
  return (
    <aside id="dock" class={min ? 'minimized' : ''}>
      <div class="dockbar">
        <span>Menu</span>
        <button type="button" title={min ? 'Show menu' : 'Hide menu'} aria-expanded={!min} onClick={(e) => { toggleDock(); e.currentTarget.blur(); }}>
          {min ? '☰ Menu' : 'Hide ▴'}
        </button>
      </div>
      <MainSection />
      <LayersSection />
      <DisplaySection />
    </aside>
  );
}
