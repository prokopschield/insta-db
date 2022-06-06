# insta-db

Instant Node.js database with built-in redundancy

### Supported platforms

This node-native module runs on 64-bit GNU/Linux and depend on git, gcc, make, node-gyp, and mmap (POSIX).

### Installation

First make sure git, gcc, and GNU Make are installed.

`yarn add insta-db`

### Usage

```typescript
import { DB } from 'insta-db';

const FILE_SIZE = 65536;
const PATH_TO_STORAGE_FILE = 'test.db';
const PATH_TO_COPY_FILE = 'backup.db'; // should be on another drive, optional

const db = new DB({
	storage_file: PATH_TO_STORAGE_FILE,
	size: FILE_SIZE,
	read_only_files: [],
	storage_copies: [PATH_TO_COPY_FILE],
});
```
