const internal = require('../build/Release/instant_db_internals');

export interface DBOptions {
    storage_file: string;
    storage_copies: string[];
    read_only_files: string[];
    size: number;
}

export type DBValue = Buffer|Uint8Array|string;

export class DB {
    private _db: any;

    constructor(opts: DBOptions)
    {
        this._db = internal.db_init({
            ...opts,
            __copies : Buffer.from(
                `${opts.storage_copies?.length}\x00${opts.storage_copies?.join('\0')}\x000\x00`),
            __rocopies : Buffer.from(
                `${
                    opts.read_only_files?.length}\x00${opts.read_only_files?.join('\0')}\x000\x00`),
        });
    }

    store(data: DBValue): string
    {
        if (data instanceof Buffer) {
            data = data;
        } else if (
            typeof data === 'string' || data?.buffer instanceof ArrayBuffer) {
            data = Buffer.from(data);
        } else {
            data = Buffer.from(String(data));
        }
        if (!data.length) {
            return '';
        }
        return this._db.store(data);
    }

    fetch(hash: string, decompress = true): Buffer|undefined
    {
        return this._db.fetch(String(hash), Boolean(decompress), false);
    }

    fetchBuffer(hash: string): Buffer|undefined
    {
        return this.fetch(hash, true);
    }

    fetchCompressed(hash: string): Buffer|undefined
    {
        return this.fetch(hash, false);
    }

    fetchString(hash: string): string
    {
        return String(this.fetchBuffer(hash));
    }

    get(key: DBValue, decompress = true): Buffer|undefined
    {
        if (typeof key !== 'string' || !key.match(/^[a-f0-9]{64}$/i)) {
            key = this.store(key);
        }
        return this._db.fetch(key, Boolean(decompress), true);
    }

    getBuffer(key: DBValue): Buffer|undefined
    {
        return this.get(key, true);
    }

    getCompressed(key: DBValue): Buffer|undefined
    {
        return this.get(key, false);
    }

    getString(key: DBValue): string
    {
        return String(this.getBuffer(key));
    }

    set(key: DBValue, val: DBValue): boolean
    {
        if (!(key instanceof Buffer)) {
            key = Buffer.from(key);
        }
        if (!(val instanceof Buffer)) {
            val = Buffer.from(val);
        }
        return this._db.associate(key, val);
    }

    private _flat?: { [index: string]: Buffer };

    get flat(): {
        [index: string]: Buffer;
    }
    {
        const db = this;
        return (
            this._flat || (this._flat = new Proxy({}, {
                get(_target, key) {
                    return db.get(String(key));
                },
                set(_target, key, val) {
                    return db.set(String(key), val);
                },
            })));
    }
}
