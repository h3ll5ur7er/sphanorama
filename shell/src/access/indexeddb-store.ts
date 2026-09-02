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

/**
 * Waits for the transaction to *commit*, which is not the same as a request succeeding.
 *
 * A write request fires `onsuccess` while its transaction is still open, and the transaction can
 * still abort afterwards — a quota check, a version change, an error in a sibling request. A
 * `flush()` built on request success would therefore report durability it had not established,
 * which is the one thing an explicit flush exists to promise.
 */
function committed(transaction: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    transaction.oncomplete = () => resolve();
    transaction.onabort = () =>
      reject(transaction.error ?? new Error('indexedDB transaction aborted'));
    transaction.onerror = () =>
      reject(transaction.error ?? new Error('indexedDB transaction failed'));
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
      const done = committed(transaction);
      transaction.objectStore(STORE).put(value, key);
      await done;
    },

    async remove(prefix: string): Promise<void> {
      const db = await connection();
      const transaction = db.transaction(STORE, 'readwrite');
      const done = committed(transaction);
      const store = transaction.objectStore(STORE);
      // The deletes are issued inside the same transaction and it commits once, so a half-removed
      // project is not a state this can leave behind.
      for (const key of await promisify(store.getAllKeys())) {
        if (String(key).startsWith(prefix)) store.delete(key);
      }
      await done;
    },
  };
}
