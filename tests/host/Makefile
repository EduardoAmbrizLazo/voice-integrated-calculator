# Host (Mac/Linux) builds of the recognizer code.  Run from this folder:
#   make            build both tests
#   make eval       firmware recognizer (src/mcu frontend/wordcap/ceps/dtw) on the recordings
#   make eval-old   the earlier single-file recognizer (src/mcu/recognizer.c)
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wshadow
MCU     := ../../src/mcu
DATA    ?= ../../src/proofofconcept/data
CPPFLAGS += -I$(MCU) -I.
FW      := $(MCU)/frontend.c $(MCU)/wordcap.c $(MCU)/ceps.c $(MCU)/dtw.c $(MCU)/recog.c

all: vic_eval rec_eval

vic_eval: vic_eval.c wav.c $(FW) $(MCU)/*.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ vic_eval.c wav.c $(FW) -lm

rec_eval: rec_eval.c wav.c $(MCU)/recognizer.c $(MCU)/recognizer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ rec_eval.c wav.c $(MCU)/recognizer.c -lm

eval: vic_eval
	./vic_eval $(DATA)

eval-old: rec_eval
	./rec_eval $(DATA)

clean:
	rm -f rec_eval vic_eval

.PHONY: all eval eval-old clean
