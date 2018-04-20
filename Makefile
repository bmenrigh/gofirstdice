CC=gccgo
GO=go

# Generic GCC performance
#CFLAGS=-Wall -Wextra -march=native -O2

# Generic GCC debugging
#CFLAGS=-Wall -Wextra -O0 -g

# Graphite loop stuff
CFLAGS=-Wall -Wextra -march=native -O3 -floop-interchange -fgraphite-identity -floop-block -floop-strip-mine -Wno-maybe-uninitialized

GOFILES=gofirstdice.go

main: gobuild

gccgo: $(GOFILES)
	$(CC) $(CFLAGS) -o gofirstdice $(GOFILES)

gobuild: $(GOFILES)
	$(GO) build $(GOFILES)

clean:
	rm -f gofirstdice
	rm -f *.o
	rm -f *~
	rm -f \#*
