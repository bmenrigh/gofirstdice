CC=gcc

CFLAGS=-Wall -march=native -O3 -std=c99 -Wextra -flto
#CFLAGS=-Wall -O2 -g
#CFLAGS=-Wall -march=native -O2 -pg
#CFLAGS=-Wall -march=native -O0 -g

main: dice diceclimber dicesmarter dicedeep dicepdeep dicemdeep dicendeep dicepldeep

dice: dice.o
	$(CC) $(CFLAGS) -o dice dice.o

dice.o: dice.c
	$(CC) $(CFLAGS) -c dice.c

diceclimber: rc4_16.o diceclimber.o pavl.o
	$(CC) $(CFLAGS) -o diceclimber rc4_16.o diceclimber.o pavl.o

diceclimber.o: diceclimber.c
	$(CC) $(CFLAGS) -c diceclimber.c

dicesmarter: rc4_16.o dicesmarter.o pavl.o
	$(CC) $(CFLAGS) -o dicesmarter rc4_16.o dicesmarter.o pavl.o -lm

dicesmarter.o: dicesmarter.c
	$(CC) $(CFLAGS) -c dicesmarter.c

dicedeep: rc4_16.o dicedeep.o pavl.o
	$(CC) $(CFLAGS) -o dicedeep rc4_16.o dicedeep.o pavl.o -lm

dicedeep.o: dicedeep.c
	$(CC) $(CFLAGS) -c dicedeep.c

dicepdeep: rc4_16.o dicepdeep.o pavl.o
	$(CC) $(CFLAGS) -o dicepdeep rc4_16.o dicepdeep.o pavl.o -lm

dicepdeep.o: dicepdeep.c
	$(CC) $(CFLAGS) -c dicepdeep.c

dicemdeep: rc4_16.o dicemdeep.o pavl.o
	$(CC) $(CFLAGS) -o dicemdeep rc4_16.o dicemdeep.o pavl.o -lm

dicemdeep.o: dicemdeep.c
	$(CC) $(CFLAGS) -c dicemdeep.c

dicendeep: rc4_16.o dicendeep.o pavl.o
	$(CC) $(CFLAGS) -o dicendeep rc4_16.o dicendeep.o pavl.o -lm

dicendeep.o: dicendeep.c
	$(CC) $(CFLAGS) -c dicendeep.c

dicepldeep: rc4_16.o dicepldeep.o pavl.o
	$(CC) $(CFLAGS) -o dicepldeep rc4_16.o dicepldeep.o pavl.o -lm

dicepldeep.o: dicepldeep.c
	$(CC) $(CFLAGS) -c dicepldeep.c

rc4_16.o: rc4_16.h rc4_16.c
	$(CC) $(CFLAGS) -c rc4_16.c

pavl.o: pavl.c pavl.h
	$(CC) $(CFLAGS) -c pavl.c

clean:
	rm -f dice diceclimber dicesmarter dicedeep dicepdeep dicemdeep dicendeep dicepldeep
	rm -f *.o
	rm -f *~
	rm -f \#*
