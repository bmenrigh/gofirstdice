// This whole program assumes column grouping.  Don't even *think* about trying to relax that restriction or Bad Things(TM) will happen.
// It also assumes all dice have the same number of sides.  You have been warned.

package main

import (
	"strings"
	"fmt"
	"sort"
)

const (
	DICE = uint8(4)
	SIDES = uint8(12)
)

var (
	PERMGOAL uint64
	TALLYGOAL uint64
	permfaircount int
	placefaircount int
	allsubsetplacefaircount int
)

var (
	dlet = [10]string{`a`, `b`, `c`, `d`, `e`, `f`, `g`, `h`, `i`, `j`} // 10 should be enough for anyone
)

type diceset [DICE][SIDES]uint8
type placetally [DICE * SIDES][DICE]uint64 // [value] then [place]
type tallytable [DICE][SIDES][DICE]uint64   // [row] then [column] then [place]
type runningtally [DICE]uint64   // [place]


// Sorting of permutations
type SortPerms []string
func (a SortPerms) Len() int           {return len(a)}
func (a SortPerms) Swap(i, j int)      {a[i], a[j] = a[j], a[i]}
func (a SortPerms) Less(i, j int) bool {if len(a[i]) == len(a[j]) {return a[i] < a[j]} else {return len(a[i]) < len(a[j])}}


func main() {

	PERMGOAL = intpow(SIDES, DICE) / factorial(DICE)
	TALLYGOAL = intpow(SIDES, DICE) / uint64(DICE)
	permfaircount = 0
	placefaircount = 0
	allsubsetplacefaircount = 0

	var dset diceset

	for d := uint8(0); d < DICE; d++ {
		for s := uint8(0); s < SIDES; s++ {
			dset[d][s] = (uint8(s) * DICE) + uint8(d)
		}
	}

	tally_table := build_tally_table()
	var min_tally, max_tally tallytable

	find_tally_left(&dset, 0, &tally_table, &min_tally, &max_tally)

	var zerotally runningtally

	for pl := uint8(0); pl < DICE; pl++ {
		zerotally[pl] = 0
	}

	fill_table_by_row(&dset, &zerotally, 0, 0, &tally_table, &min_tally, &max_tally)

	//fmt.Printf("Got str: %s\n", strings.Join(setstring(dset), ``))

	//printperms(findperms(strings.Split("abcdeecdabdbaececdabbecdadbcaeacbdeecdabbeacdeabdcdacebbacdedcaebacbdeabcdeedcbaedbcabeacdedcabbecadcdbaedcaebbadceedbcaeacbdadcebbadceceabdbadceedcba", ``)))

	//isallsubsetplacefair(findperms(strings.Split("abcddcbadbcaacbdcbdaadbccbdaadbcdbcaacbdabdccdba", ``)))
	//isallsubsetplacefair(findperms(strings.Split("abcddcbadbcacabdabcddcbaadcbbcdabdacacdbadbccbda", ``)))

	//fmt.Printf("%d, %d : %d\n", 12, 3, binomial(12,3))

	fmt.Printf("Place fair: %d; All subset place fair: %d; Perm fair: %d\n", placefaircount, allsubsetplacefaircount, permfaircount)

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


func ispermfair(perms map[string]int) bool {

	// Permutations of length l
	for l := uint8(2); l <= DICE; l++ {

		permtarget := intpow(SIDES, l) / factorial(l)

		for p := range perms {
			if len(p) == int(l) {
				if uint64(perms[p]) != permtarget {
					return false
				}
			}
		}
	}

	return true
}


func isallsubsetplacefair(perms map[string]int) bool {

	subsets := make(map[string]map[string][]uint64)
	var subsetgoals [DICE - 1]uint64

	for i := uint8(0); i < DICE - 1; i++ {
		subsetgoals[i] = intpow(SIDES, i + 2) / (uint64(i) + 2)
	}

	for p := range perms {
		l := len(p)

		if l > 1 {

			letlist := strings.Split(p, ``)
			sort.Strings(letlist)
			subset := strings.Join(letlist, ``)

			if _, ok := subsets[subset]; ok == false {
				subsets[subset] = make(map[string][]uint64)


				for _, a := range letlist {
					subsets[subset][a] = make([]uint64, l)
				}
			}

			for i := 0; i < l; i++ {
				a := p[i:i + 1]

				subsets[subset][a][i] += uint64(perms[p])
			}
		}
	}

	for subset := range subsets {
		for a := range subsets[subset] {

			l := len(subset)
			for _, s := range subsets[subset][a] {
				if s != subsetgoals[l - 2] {

					//fmt.Printf("Subset %s with letter %s failed. Got %d, expected %d\n",
					//	subset, a, s, subsetgoals[l - 2])

					return false
				}
			}
		}
	}

	return true
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
	min_tally *tallytable, max_tally *tallytable) {

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


func fill_table_by_row(dset *diceset, curtally *runningtally, row uint8, side uint8,
	tally_table *placetally, min_tally *tallytable, max_tally *tallytable) {


	// We've gotten to the end of a dice, move on to the next or check results
	if side >= SIDES {

		// Check for place fairness
		for pl := uint8(0); pl < DICE; pl ++ {
			if (*curtally)[pl] != TALLYGOAL {
				return
			}
		}

		// If there are any dice left, move on to them
		if row < (DICE - 1) {
			var zerotally runningtally

			for pl := uint8(0); pl < DICE; pl++ {
				zerotally[pl] = 0
			}

			fill_table_by_row(dset, &zerotally, row + 1, 0, tally_table, min_tally, max_tally)

			return;
		} else {
			// This means we have now filled out all the dice

			dstr := setstring(*dset)
			//fmt.Printf("Got placefair set: %s\n", strings.Join(dstr, ``))
			placefaircount++

			perms := findperms(dstr)

			if ispermfair(perms) {
				permfaircount++
				fmt.Printf("Got permfair set: %s\n", strings.Join(dstr, ``))
			} else if isallsubsetplacefair(perms) {
				allsubsetplacefaircount++
				fmt.Printf("Got allsubsetplacefair set: %s\n", strings.Join(dstr, ``))
			}


			return
		}
	}

	// Each time we start on a new row build the tally table min/max using the previous rows
	if side == 0 && row != DICE {
		find_tally_left(dset, row, tally_table, min_tally, max_tally)
	}

	// Try to prune based on the runny tally and the min/max
	for pl := uint8(0); pl < DICE; pl++ {
		// If the current running tally plus the max doesn't reach our goal
		if (*curtally)[pl] + max_tally[row][side][pl] < TALLYGOAL {
			return
		}

		// If the current running tally plus the min goes over our goal
		if (*curtally)[pl] + min_tally[row][side][pl] > TALLYGOAL {
			return
		}
	}

	// At this point we are still in the recursion and need to fill out
	// more columns for this row

	for d := row; d < DICE; d++ {

		// To hold the new diceset each time we change it
		// Copy current dice set to new one
		var newd diceset
		for nd := uint8(0); nd < DICE; nd++ {
			for ns := uint8(0); ns < SIDES; ns++ {
				newd[nd][ns] = (*dset)[nd][ns]
			}
		}

		// Swap out the cell in this row with one below
		newd[row][side] = (*dset)[d][side]
		newd[d][side] = (*dset)[row][side]

		// Duplicate the running tally table to local copy
		var newtally runningtally
		for pl:= uint8(0); pl < DICE; pl++ {
			// Now that we've chosen a value, add it to our running tallies
			newtally[pl] = (*curtally)[pl] + tally_table[newd[row][side]][pl];
		}

		fill_table_by_row(&newd, &newtally, row, side + 1, tally_table, min_tally, max_tally)

		// If this is the first column we don't permute it
		if side == 0 {
			break
		}
	} // End swapping in of lower cells in this column
}