CC ?= cc

DICE ?= 4
SIDES ?= 12
MIRROR ?= 0

CPPFLAGS += -DDICE=$(DICE) -DSIDES=$(SIDES) -DMIRROR=$(MIRROR)
CFLAGS ?= -O3 -march=native -flto
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -pthread

column_search: column_search.c FORCE
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

.PHONY: FORCE clean
FORCE:

clean:
	rm -f column_search
