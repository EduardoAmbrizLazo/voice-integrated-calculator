# Host (Mac/Linux) build of the recognizer.  Run from this folder:
#   make            build
#   make eval       accuracy on src/proofofconcept/data
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
MCU     := ../../src/mcu
DATA    ?= ../../src/proofofconcept/data
CPPFLAGS += -I$(MCU) -I.

all: rec_eval

rec_eval: rec_eval.c wav.c $(MCU)/recognizer.c $(MCU)/recognizer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ rec_eval.c wav.c $(MCU)/recognizer.c -lm

eval: rec_eval
	./rec_eval $(DATA)

clean:
	rm -f rec_eval

.PHONY: all eval clean
