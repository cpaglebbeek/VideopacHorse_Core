# VideopacHorse_Core — build
# Doelen:
#   make            -> libg7000.a (native, release)
#   make test       -> bouwt + draait volledige testsuite (met sanitizers)
#   make wasm       -> build/wasm/g7000.{js,wasm} via emcc (voor VideopacHorse_Web)
#   make clean

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Werror -Iinclude
TESTCFLAGS = -std=c11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc \
             -fsanitize=address,undefined -fno-omit-frame-pointer

SRC  := $(wildcard src/*.c)
OBJ  := $(SRC:src/%.c=build/obj/%.o)
TSRC := $(wildcard tests/*.c)

all: build/libg7000.a

build/obj/%.o: src/%.c include/g7000.h
	@mkdir -p build/obj
	$(CC) $(CFLAGS) -c $< -o $@

build/libg7000.a: $(OBJ)
	$(AR) rcs $@ $^

test: $(SRC) $(TSRC)
	@mkdir -p build
	$(CC) $(TESTCFLAGS) $(SRC) $(TSRC) -o build/g7k_test
	./build/g7k_test

wasm: $(SRC)
	@mkdir -p build/wasm
	emcc -std=c11 -O3 -Iinclude $(SRC) \
	  -sEXPORTED_FUNCTIONS=_g7k_create,_g7k_destroy,_g7k_load_bios,_g7k_load_cart,_g7k_reset,_g7k_set_region,_g7k_run_frame,_g7k_framebuffer,_g7k_fb_width,_g7k_fb_height,_g7k_audio_read,_g7k_audio_sample_rate,_g7k_joystick_set,_g7k_key_set,_g7k_key_from_char,_g7k_state_size,_g7k_state_save,_g7k_state_load,_g7k_version,_malloc,_free \
	  -sEXPORTED_RUNTIME_METHODS=cwrap,HEAPU8,HEAP16,HEAPU32 \
	  -sMODULARIZE=1 -sEXPORT_NAME=createG7000 -sALLOW_MEMORY_GROWTH=1 \
	  -o build/wasm/g7000.js

clean:
	rm -rf build

.PHONY: all test wasm clean
