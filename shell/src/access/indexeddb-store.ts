/**
 * IndexedDB behind the document host.
 *
 * Deliberately thin, and deliberately untested by unit tests: a test asserting that IndexedDB
 * stores values is testing the browser (docs/00 §0.2). What is worth testing is the host's
 * resident model, which has its own suite, and that a project survives a reload — which the
 * end-to-end suite checks in a real browser.
 */
import type { DocumentStore } from './document-host';

const DATABASE = 'sphanorama';
const STORE = 'documents';
const VERSION = 1;

function open(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DATABASE, VERSION);
    request.onupgradeneeded = () => {
      if (!request.result.objectStoreNames.contains(STORE)) request.result.createObjectStore(STORE);
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error('indexedDB open failed'));
    request.onblocked = () => reject(new Error('indexedDB open blocked by another tab'));
  });
}

function promisify<T>(request: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error ?? new Error('indexedDB request failed'));
  });
}

export function createIndexedDbStore(): DocumentStore {
  // Opened lazily and kept: the host hydrates once at startup and writes through afterwards.
  let database: Promise<IDBDatabase> | null = null;
  const connection = () => (database ??= open());

  return {
    async loadAll(): Promise<Record<string, string>> {
      const db = await connection();
      const transaction = db.transaction(STORE, 'readonly');
      const store = transaction.objectStore(STORE);
      const [keys, values] = await Promise.all([
        promisify(store.getAllKeys()),
        promisify(store.getAll()),
      ]);
      const out: Record<string, string> = {};
      keys.forEach((key, index) => {
        out[String(key)] = String(values[index]);
      });
      return out;
    },

    async put(key: string, value: string): Promise<void> {
      const db = await connection();
      const transaction = db.transaction(STORE, 'readwrite');
      await promisify(transaction.objectStore(STORE).put(value, key));
    },

    async remove(prefix: string): Promise<void> {
      const db = await connection();
      const transaction = db.transaction(STORE, 'readwrite');
      const store = transaction.objectStore(STORE);
      for (const key of await promisify(store.getAllKeys())) {
        if (String(key).startsWith(prefix)) await promisify(store.delete(key));
      }
    },
  };
}
