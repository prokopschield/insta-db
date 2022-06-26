all: build/Makefile build/blake3-build/libblake3.a build/libdeflate build/Release/db.node lib/index.js

run: all
	node .

fmt:
	clang-format --style=WebKit -i src/*

build/Makefile:
	yarn

build/Release/db.node: $(wildcard src/*.cc)
	node-gyp build

lib/index.js: $(wildcard src/*.ts)
	tsc

build/blake3-build/libblake3.a:
	cd build && git clone https://github.com/prokopschield/blake3-build && cd blake3-build && make

build/libdeflate:
	cd build && git clone --depth 1 https://github.com/prokopschield/libdeflate && cd libdeflate && make

clean:
	rm -rf build/ lib/ node_modules/
