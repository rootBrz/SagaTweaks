CC     = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -s -static-libgcc

all: build/SagaTweaks.asi

build:
	mkdir -p build

build/SagaTweaks.asi: src/tweaks.c | build
	$(CC) $(CFLAGS) -shared -o $@ src/tweaks.c

dist: all
	cd build && zip -q SagaTweaks.zip SagaTweaks.asi

clean:
	rm -rf build

.PHONY: all dist clean
