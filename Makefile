CC     = i686-w64-mingw32-gcc
VERSION := $(shell tr -d "[:space:]" < VERSION)
CFLAGS = -O2 -Wall -Wextra -MMD -MP -Isrc -DSAGA_VERSION=\"$(VERSION)\"
LDFLAGS = -shared -s -static-libgcc

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/obj/%.o)

all: build/SagaTweaks.asi

build/SagaTweaks.asi: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

build/obj/main.o: VERSION

build/obj/%.o: src/%.c | build/obj
	$(CC) $(CFLAGS) -c -o $@ $<

build/obj:
	mkdir -p $@

dist: all
	rm -f build/SagaTweaks.7z
	cd build && 7z a -bso0 SagaTweaks.7z SagaTweaks.asi ../LICENSE ../README.md ../CHANGELOG.md

clean:
	rm -rf build

-include $(OBJ:.o=.d)

.PHONY: all dist clean
