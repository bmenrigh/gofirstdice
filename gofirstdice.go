// This whole program assumes column grouping.  Don't even *think* about trying to relax that restriction or Bad Things(TM) will happen.
// It also assumes all dice have the same number of sides.  You have been warned.

package main

import (
	"strings"
	"fmt"
	"sort"
)

const (
	DICE = uint8(5)
	SIDES = uint8(30)
)

var (
	PERMGOAL uint64
	TALLYGOAL uint64
)

var (
	dlet = [10]string{`a`, `b`, `c`, `d`, `e`, `f`, `g`, `h`, `i`, `j`} // 10 should be enough for anyone
)

type diceset [DICE][SIDES]uint8
type placetally [DICE * SIDES][DICE]uint64 // [value] then [place]
type tallyleft [DICE][SIDES][DICE]uint64   // [row] then [column] then [place]


// Sorting of permutations
type SortPerms []string
func (a SortPerms) Len() int           {return len(a)}
func (a SortPerms) Swap(i, j int)      {a[i], a[j] = a[j], a[i]}
func (a SortPerms) Less(i, j int) bool {if len(a[i]) == len(a[j]) {return a[i] < a[j]} else {return len(a[i]) < len(a[j])}}


func main() {

	PERMGOAL = intpow(SIDES, DICE) / factorial(DICE)
	TALLYGOAL = intpow(SIDES, DICE) / uint64(DICE)

	tally_table := build_tally_table()
	var min_tally, max_tally tallyleft

	fmt.Printf("tally 0 place 0: %d\n", tally_table[0][0])

	var dset diceset

	for d := uint8(0); d < DICE; d++ {
		for s := uint8(0); s < SIDES; s++ {
			dset[d][s] = (uint8(s) * DICE) + uint8(d)
		}
	}

	find_tally_left(&dset, 0, &tally_table, &min_tally, &max_tally)

	fmt.Printf("Got str: %s\n", strings.Join(setstring(dset), ``))

	//printperms(findperms(strings.Split("abcdeecdabdbaececdabbecdadbcaeacbdeecdabbeacdeabdcdacebbacdedcaebacbdeabcdeedcbaedbcabeacdedcabbecadcdbaedcaebbadceedbcaeacbdadcebbadceceabdbadceedcba", ``)))

	//fmt.Printf("%d, %d : %d\n", 12, 3, binomial(12,3))

}


func setstring(dset diceset) []string {

	dstr := make([]string, DICE * SIDES)

	for i := uint8(0); i < (DICE * SIDES); i++ {
		s := i / DICE;
		for d := uint8(0); d < DICE; d++ {
			if dset[d][s] == i {
				dstr[i] = dlet[d]
				break
			}
		}
	}

	return dstr
}


func findperms(dstr []string) map[string]int {

	// we store the permutation counts here
	perms := make(map[string]int)

	// One way to permute nothing
	perms[``] = 1

	for _, l := range dstr {
		for p, c := range perms {
			if strings.Index(p, l) < 0 {
				perms[p + l] += c
			}
		}
	}

	return perms
}


func printperms(perms map[string]int) {

	var pnames []string
	for p := range perms {
		pnames = append(pnames, p)
	}

	sort.Sort(SortPerms(pnames))

	for _, p := range pnames {
		fmt.Printf("\"%s\" -> %d\n", p, perms[p])
	}
}


// Slow but this doesn't get used much
// Languages really should provide integer pow()
func intpow(n uint8, e uint8) uint64 {

	if e == 0 {
		return 1
	}

	bn := uint64(n)
	c := bn
	for i := e; i > 1; i-- {
		c *= bn
	}

	return c
}


func factorial(n uint8) uint64 {

	if n <= 1 {
		return 1
	}

	c := uint64(1)
	for i := uint64(n); i > 1; i-- {
		c *= i
	}

	return c
}


func binomial(n uint8, r uint8) uint64 {

	if r > n {
		return 0
	}
	if n == r {
		return 1
	}

	var l1, l2 uint64
	// Figure out the larger limit
	if (n - r) > r {
		l1 = uint64(r)
		l2 = uint64(n - r)
	} else {
		l1 = uint64(n - r)
		l2 = uint64(r)
	}

	// First multiply n * n - 1 * n - 2 ... down to the limit
	c := uint64(1)
	for i := uint64(n); i > l1; i-- {
		c *= i
	}
	// Now divide out the other limit
	for i := uint64(l2); i > 1; i-- {
		c /= i
	}

	return c
}


func build_tally_table() placetally {

	var tally_cache placetally

	for s := uint8(0); s < SIDES; s++ {
		for d := uint8(0); d < DICE; d++ {

			// v is the dice side value
			v := (s * DICE) + d;

			// Now figure out for each place, starting with first
			for p := uint8(0); p < DICE; p++ {
				// to be in 1st place (0th) there needs to be DICE - 1 numbers under
				// and 0 nums above

				// to be in p'th place there needs to be (DICE - 1) - p under
				// and p over you

				// You can choose dice - 1 choose n ways to pick from the
				// before or after dice

				// abv is the number of faces to choose from the rows above this d
				for abv := uint8(0); ((abv <= p) && (abv <= d)); abv++ {
					tally_cache[v][p] +=
						binomial(d, abv) *
						intpow((SIDES - s) - 1, abv) *
						binomial((DICE - 1) - d, p - abv) *
						intpow(SIDES - s, p - abv) *
						intpow(s + 1, d - abv) *
						intpow(s, ((DICE - d) - 1) - (p - abv));
				}

				// fmt.Printf("n=%d, p=%d, c=%d\n", (s * DICE) + d, p, tally_cache[v][p]);
			}
		}
	}

	return tally_cache
}


func find_tally_left(dset *diceset, row uint8, tally_table *placetally,
	min_tally *tallyleft, max_tally *tallyleft) {

	l := SIDES // The limit

	for p := uint8(0); p < DICE; p++ { // for each place
		for s := l; s >= 1; s-- { // start from last to first
			min := intpow(SIDES, DICE)
			max := uint64(0)

			for d := row; d < DICE; d++ { // for each dice you could pick
				v := (*dset)[d][s - 1]; // The value

				t := tally_table[v][p]

				if (t < min) {
					min = t
				}
				if (t > max) {
					max = t
				}
			} // end for d

			//fprintf(stderr, "Adding min,max for s=%d; p=%d to %lu, %lu\n", s, p, min, max);*/
			//fprintf(stderr, "at r=%d; s=%d; p=%d; min=%lu; max=%lu\n", rows, s, p, min, max);*/

			// If this is the last column it doesn't get added to the next higher column
			if (s == l) {
				(*min_tally)[row][s - 1][p] = min;
				(*max_tally)[row][s - 1][p] = max;
			} else {
				(*min_tally)[row][s - 1][p] = min + (*min_tally)[row][s][p];
				(*max_tally)[row][s - 1][p] = max + (*max_tally)[row][s][p];
			}
			// fmt.Printf("Bringing min,max for s=%d; p=%d to %d, %d\n", s, p, (*min_tally)[row][s - 1][p], (*max_tally)[row][s - 1][p]);

		} // end for s
	}
}
