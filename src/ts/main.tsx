// Entry point: renders the Preact UI (src/ts/ui) and starts the app. All parsing and rendering happens in C++ (WebAssembly).
import { render } from 'preact';
import { App } from './ui/App.tsx';
import { start } from './ui/controller.ts';

render(<App />, document.getElementById('root') as HTMLElement);
start();
