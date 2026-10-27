// The page: the menu, the minigame overlay, the marker label and the full-window overlays. The canvas lives in index.html.
import { useEffect } from 'preact/hooks';
import { Dock } from './Dock.tsx';
import { GameHud } from './GameHud.tsx';
import { Overlays } from './Overlays.tsx';
import { useUi } from './store.ts';
import { Tooltip } from './Tooltip.tsx';

export function App() {
  const playing = useUi((s) => s.gameMode);
  const dragging = useUi((s) => s.dragging);
  useEffect(() => { document.body.classList.toggle('playing', playing); }, [playing]);
  useEffect(() => { document.body.classList.toggle('dragging', dragging); }, [dragging]);
  return (
    <>
      <Dock />
      <GameHud />
      <Tooltip />
      <Overlays />
    </>
  );
}
