// Browser APIs that lib.dom does not describe yet (File System Access) and the globals the page shares with the C++ side.
import type { RmeApi, RmeModule } from './types.ts';

declare global {
  interface Window {
    RME: RmeApi;
    Module: Partial<RmeModule>;
    showDirectoryPicker?: (options?: { mode?: 'read' | 'readwrite' }) => Promise<FileSystemDirectoryHandle>;
  }
  interface FileSystemDirectoryHandle {
    queryPermission(descriptor?: { mode?: 'read' | 'readwrite' }): Promise<PermissionState>;
    requestPermission(descriptor?: { mode?: 'read' | 'readwrite' }): Promise<PermissionState>;
  }
  interface DataTransferItem {
    getAsFileSystemHandle?: () => Promise<FileSystemHandle | null>;
  }
}
