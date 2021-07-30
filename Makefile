#! Makefile for the semWait
# Sun Jul 25 21:34:21 EDT 2021
.PHONY: clean
all: semWait


LINKERFLAG = -lm

SRCS := $(wildcard *.c)
BINS := $(SRCS:%.c=%)

%: %.o
		@echo "Checking.."
		${CC} -g ${LINKERFLAG} $< -o $@

%.o: %.c
		@echo "Creating object.."
		${CC} -g -c $<


clean: 
	rm -f semWait
	rm -f *.o

#
