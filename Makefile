CC ?= clang
OPT ?= -O2
CFLAGS = $(OPT) -g -fno-strict-aliasing -fwrapv -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable \
         -Wno-unused-function -Wno-parentheses-equality -Iruntime $(shell sdl2-config --cflags)
LDLIBS = $(shell sdl2-config --libs) -lGLESv2 -lm -lpthread -lz
GEN_SRC = $(wildcard gen/exe/*.c gen/bdd/*.c)
RT_SRC = runtime/rt.c runtime/heap.c runtime/game_glue.c $(wildcard win32/*.c) \
         platform/plat_sdl.c platform/settings.c platform/main_desktop.c platform/touch.c platform/audio.c
OBJ = $(patsubst %.c,build/%.o,$(GEN_SRC) $(RT_SRC))

carma2: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

build/gen/%.o: gen/%.c runtime/lifted.h runtime/cpu.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(dir $<) -c -o $@ $<

build/%.o: %.c runtime/*.h win32/*.h platform/*.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build carma2
