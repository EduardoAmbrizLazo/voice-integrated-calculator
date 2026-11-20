# Host (Mac/Linux) builds of the recognizer code.  Run from this folder:
#   make            build both tests
#   make eval       calculator and clock/timer/mode checks, then the firmware recognizer (frontend/wordcap/ceps/dtw/
#                   enroll/calc in src/mcu) on the recordings, flash save/load and spoken sums
#   make eval-old   the earlier single-file recognizer (src/mcu/recognizer.c)
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wshadow
MCU     := ../../src/mcu
DATA    ?= ../../src/proofofconcept/data
CPPFLAGS += -I$(MCU) -I.
FW      := $(MCU)/frontend.c $(MCU)/wordcap.c $(MCU)/ceps.c $(MCU)/dtw.c $(MCU)/recog.c \
           $(MCU)/enroll.c $(MCU)/calc.c

all: vic_eval calc_test modes_test rec_eval

vic_eval: vic_eval.c wav.c $(FW) $(MCU)/*.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ vic_eval.c wav.c $(FW) -lm

rec_eval: rec_eval.c wav.c $(MCU)/recognizer.c $(MCU)/recognizer.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ rec_eval.c wav.c $(MCU)/recognizer.c -lm

calc_test: calc_test.c $(MCU)/calc.c $(MCU)/calc.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ calc_test.c $(MCU)/calc.c

modes_test: modes_test.c $(MCU)/modes.c $(MCU)/calc.c $(MCU)/timer.c $(MCU)/clock.c $(MCU)/*.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ modes_test.c $(MCU)/modes.c $(MCU)/calc.c $(MCU)/timer.c $(MCU)/clock.c

eval: vic_eval calc_test modes_test
	./calc_test
	./modes_test
	./vic_eval $(DATA)

eval-old: rec_eval
	./rec_eval $(DATA)

clean:
	rm -f rec_eval vic_eval calc_test modes_test

.PHONY: all eval eval-old clean
