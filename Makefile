CC     = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -s -static-libgcc

all: build/dinput8.dll build/SagaTweaks.asi

build:
	mkdir -p build

build/dinput8.dll: src/loader.c src/dinput8.def | build
	$(CC) $(CFLAGS) -shared -o $@ src/loader.c src/dinput8.def -Wl,--kill-at

build/SagaTweaks.asi: src/tweaks.c | build
	$(CC) $(CFLAGS) -shared -o $@ src/tweaks.c

dist: all
	cd build && zip -q SagaTweaks.zip dinput8.dll SagaTweaks.asi

clean:
	rm -rf build

.PHONY: all dist clean
