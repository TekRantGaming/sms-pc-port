# Host test for the THP decoders (decomp THPDec.c/THPAudio.c + thp-0*.patch).
#   make run DISC=/path/to/GMSE01/files [MOVIE=data/openingA.thp] [FRAMES=...]
PORT   := $(abspath ../../..)
DECOMP := $(PORT)/decomp
ARCH   ?= -m32
CC     ?= gcc
CXX    ?= g++
CFLAGS := $(ARCH) -O2 -g -std=gnu99 -w -fno-strict-aliasing -DTARGET_PC -DGEKKO \
          -I$(PORT)/src/port_include -I$(DECOMP)/include -I$(DECOMP)/libs/dolphin/include -include $(PORT)/src/port_compat.h
# Patched copies and objects live outside platform/ (the port globs platform/**/*.c).
WORK   ?= $(PORT)/build-thp-test
OUT    ?= $(WORK)/out

.DEFAULT_GOAL := run

$(WORK)/patched/.stamp: $(wildcard $(PORT)/decomp-patches/thp-0[1235]-*.patch) $(DECOMP)/libs/dolphin/src/thp/THPDec.c $(DECOMP)/libs/dolphin/src/thp/THPAudio.c
	rm -rf $(WORK)/patched && mkdir -p $(WORK)/patched/libs/dolphin/src/thp
	cp $(DECOMP)/libs/dolphin/src/thp/THPDec.c $(DECOMP)/libs/dolphin/src/thp/THPAudio.c $(WORK)/patched/libs/dolphin/src/thp/
	for p in $(PORT)/decomp-patches/thp-0[1235]-*.patch; do patch -p1 --quiet -d $(WORK)/patched -i $$p || exit 1; done
	touch $@

$(WORK)/thp_test: thp_test.cc $(WORK)/patched/.stamp
	$(CC) $(CFLAGS) -c -o $(WORK)/THPDec.o $(WORK)/patched/libs/dolphin/src/thp/THPDec.c
	$(CC) $(CFLAGS) -c -o $(WORK)/THPAudio.o $(WORK)/patched/libs/dolphin/src/thp/THPAudio.c
	$(CXX) $(ARCH) -O2 -g -std=gnu++11 -o $@ thp_test.cc $(WORK)/THPDec.o $(WORK)/THPAudio.o -lm

run: $(WORK)/thp_test
	mkdir -p $(OUT) && $(WORK)/thp_test $(OUT)

clean:
	rm -rf $(WORK)
.PHONY: run clean

# The draw test uses patterned source planes and verifies every submitted
# texel and display edge, including tall movies and the 2048x2048 limit.
tiles:
	mkdir -p $(WORK)
	$(CXX) $(ARCH) -O2 -std=gnu++11 -DTARGET_PC -DGEKKO \
	  -I$(PORT)/src -I$(PORT)/src/port_include -I$(DECOMP)/libs/dolphin/include \
	  -I$(DECOMP)/libs/THPPlayer/include -o $(WORK)/tile_test \
	  tile_test.cc $(PORT)/platform/thp/hd_movies.cpp
	$(WORK)/tile_test
.PHONY: tiles
