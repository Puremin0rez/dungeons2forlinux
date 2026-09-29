# Needs a MinGW-w64 cross compiler (mingw-w64-gcc or llvm-mingw); tests also need Wine.
CC := $(shell command -v x86_64-w64-mingw32-clang 2>/dev/null || command -v x86_64-w64-mingw32-gcc 2>/dev/null || echo x86_64-w64-mingw32-gcc)
CFLAGS := -O2 -Wall -Wextra -Wshadow -Wno-cast-function-type
LDFLAGS := -shared -s -static-libgcc -Wl,--no-insert-timestamp
BUILD := build
VERSION ?= $(shell git describe --tags --match 'v*' --always --dirty 2>/dev/null || echo dev)
PKG := dungeons2forlinux-$(VERSION)
DEFINES := -DD2FL_VERSION=\"$(VERSION)\"

.PHONY: all test test-python test-release package clean FORCE

all: dist/xgameruntime.dll

# Changes only when VERSION does, so a new version rebuilds the DLLs.
$(BUILD)/version: FORCE
	@mkdir -p $(BUILD)
	@echo '$(VERSION)' | cmp -s - $@ || echo '$(VERSION)' > $@

dist/xgameruntime.dll: src/xgameruntime.c src/signin.py $(BUILD)/version
	@mkdir -p dist
	$(CC) $(CFLAGS) $(DEFINES) $(LDFLAGS) -o $@ $<

# Test build: freed memory faults on access.
$(BUILD)/xgameruntime.dll: src/xgameruntime.c src/signin.py tests/guard_heap.h $(BUILD)/version
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -Wall -Wno-cast-function-type $(DEFINES) -shared -include tests/guard_heap.h -o $@ $<

$(BUILD)/test_runtime.exe: tests/test_runtime.c tests/guard_heap.h
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -Wall -include tests/guard_heap.h -o $@ $<

# Stand-in for the game's XCurl.dll.
$(BUILD)/XCurl.dll: tests/fake_xcurl.c
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -Wall -shared -o $@ $< -lwinhttp

test-python:
	python3 -m unittest discover -s tests -p 'test_signin.py'

test: test-python $(BUILD)/xgameruntime.dll $(BUILD)/test_runtime.exe $(BUILD)/XCurl.dll
	tests/run.sh $(BUILD)

# Runs the Wine tests against the release DLL.
test-release: dist/xgameruntime.dll $(BUILD)/test_runtime.exe $(BUILD)/XCurl.dll
	@mkdir -p $(BUILD)/release
	cp dist/xgameruntime.dll $(BUILD)/test_runtime.exe $(BUILD)/XCurl.dll $(BUILD)/release/
	tests/run.sh $(BUILD)/release

# build/dungeons2forlinux-VERSION.tar.gz and its .sha256, byte-identical for the same commit.
package: dist/xgameruntime.dll
	rm -rf $(BUILD)/$(PKG)
	mkdir -p $(BUILD)/$(PKG)
	cp install.sh README.md LICENSE dist/xgameruntime.dll $(BUILD)/$(PKG)/
	chmod 0755 $(BUILD)/$(PKG)/install.sh
	chmod 0644 $(BUILD)/$(PKG)/README.md $(BUILD)/$(PKG)/LICENSE $(BUILD)/$(PKG)/xgameruntime.dll
	tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -C $(BUILD) -cf - $(PKG) | gzip -9n > $(BUILD)/$(PKG).tar.gz
	cd $(BUILD) && sha256sum $(PKG).tar.gz > $(PKG).tar.gz.sha256

clean:
	rm -rf $(BUILD) dist
