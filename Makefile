CC     = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -MMD -MP -Isrc
LDFLAGS = -shared -s -static-libgcc

SRC := $(wildcard src/*.c)
OBJ := $(SRC:src/%.c=build/obj/%.o)

all: build/SagaTweaks.asi

build/SagaTweaks.asi: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

build/obj/%.o: src/%.c | build/obj
	$(CC) $(CFLAGS) -c -o $@ $<

build/obj:
	mkdir -p $@

dist: all
	cd build && zip -q SagaTweaks.zip SagaTweaks.asi

clean:
	rm -rf build

-include $(OBJ:.o=.d)

.PHONY: all dist clean
