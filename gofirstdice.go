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
	SIDES = uint8(18)
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

	//fmt.Printf("Got str: %s\n", strings.Join(setstring(dset, DICE), ``))

	//printperms(findperms(strings.Split("abcdeecdabdbaececdabbecdadbcaeacbdeecdabbeacdeabdcdacebbacdedcaebacbdeabcdeedcbaedbcabeacdedcabbecadcdbaedcaebbadceedbcaeacbdadcebbadceceabdbadceedcba", ``)))

	//isallsubsetplacefair(findperms(strings.Split("abcddcbadbcaacbdcbdaadbccbdaadbcdbcaacbdabdccdba", ``)))
	//isallsubsetplacefair(findperms(strings.Split("abcddcbadbcacabdabcddcbaadcbbcdabdacacdbadbccbda", ``)))

	//fmt.Printf("%d, %d : %d\n", 12, 3, binomial(12,3))

	fmt.Printf("Place fair: %d; All subset place fair: %d; Perm fair: %d\n", placefaircount, allsubsetplacefaircount, permfaircount)

}


func setstring(dset diceset, dcount uint8) []string {

	dstr := make([]string, DICE * SIDES)

	l := 0 // track the length of the string made
	for v := uint8(0); v < (DICE * SIDES); v++ {
		s := v / DICE;
		for d := uint8(0); d < dcount; d++ {
			if dset[d][s] == v {
				dstr[l] = dlet[d]
				l++
				break
			}
		}
	}

	return dstr[0:l]
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


func ispermfair(perms map[string]int, dcount uint8) bool {

	// Permutations of length l
	for l := uint8(2); l <= dcount; l++ {

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


func isallsubsetplacefair(perms map[string]int, dcount uint8) bool {

	// A subset is an ordered set of letters like acd
	// The second map is an individual letter like c
	// The slice tracks the number of times the letter shows
	// up in a given place
	subsets := make(map[string]map[string][]uint64) // [subset][letter][place]

	// Computer the goal sum for each place for each letter in each subset
	var subsetgoals [DICE - 1]uint64
	for i := uint8(0); i < dcount - 1; i++ {
		subsetgoals[i] = intpow(SIDES, i + 2) / (uint64(i) + 2)
	}

	// For each full permutation like abc, bac, acb, etc.
	for p := range perms {
		l := len(p)

		// Skip the empty perm and the singe-die perms
		if l > 1 {

			// Get a list of the letters and sort them
			// to get which subset this is
			letlist := strings.Split(p, ``)
			sort.Strings(letlist)
			subset := strings.Join(letlist, ``)

			// If we haven't seen this subset yet then create
			// the additional letter map and place slice
			if _, ok := subsets[subset]; ok == false {
				subsets[subset] = make(map[string][]uint64)

				for _, a := range letlist {
					subsets[subset][a] = make([]uint64, l)
				}
			}

			// Now for each letter in this permutation, record
			// how many times it shows up in the Nth position
			// in this subset
			for i := 0; i < l; i++ {
				a := p[i:i + 1] // Grap out just the single letter at this index

				subsets[subset][a][i] += uint64(perms[p])
			}
		}
	}

	// Now for each subset
	for subset := range subsets {
		// And each letter within the subset
		for a := range subsets[subset] {

			l := len(subset)
			// Make sure each Nth place this letter shows up
			// is equal to the goal we expect for a fully place fair subset
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


func checkpairsfair(dset *diceset, d1 uint8, d2 uint8) bool {

	// This function checks to see if d1 and d2 are pairwise-fair
	c := uint8(0)
	for s := uint8(0); s < SIDES; s++ {
		if (*dset)[d1][s] > (*dset)[d2][s] {
			c++
		}
	}

	if c == SIDES / 2 {
		return true
	}

	return false
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

	// Make local copies of the min/max tally tables
	var loc_min_tally, loc_max_tally tallytable
	for d := uint8(0); d < DICE; d++ {
		for s := uint8(0); s < SIDES; s++ {
			for pl := uint8(0); pl < DICE; pl++ {
				loc_min_tally[d][s][pl] = (*min_tally)[d][s][pl]
				loc_max_tally[d][s][pl] = (*max_tally)[d][s][pl]
			}
		}
	}

	// Pre-allocate a runningtally for each recursion call so we
	// don't have to constantly make a new one and have the GC
	// constantly collecting them
	var curtallycache[DICE][SIDES + 1]runningtally

	// Copy the passed curtally into the cache
	for pl := uint8(0); pl < DICE; pl++ {
		curtallycache[row][side][pl] = (*curtally)[pl]
	}

	// Pre-allocate a dset for each recursion call too
	var dsetcache[DICE][SIDES + 1]diceset

	// Copy the passed dset into the cache
	for d := uint8(0); d < DICE; d++ {
		for s := uint8(0); s < SIDES; s++ {
			dsetcache[row][side][d][s] = (*dset)[d][s]
		}
	}

	var search func(*diceset, *runningtally, uint8, uint8)
	search = func(dset *diceset, curtally *runningtally, row uint8, side uint8) {

		// We've gotten to the end of a dice, move on to the next or check results
		if side >= SIDES {

			// Check for place fairness
			for pl := uint8(0); pl < DICE; pl ++ {
				if (*curtally)[pl] != TALLYGOAL {
					return
				}
			}

			// Check pairwise fairness up to this point
			// This check is much faster than checking all
			// subset fairness so we do that first
			for d1 := uint8(0); d1 < row; d1++ {
				if checkpairsfair(dset, d1, row) == false {
					return
				}
			}

			// Now we can check that all subsets are fair up to here
			if row > 1 && row < (DICE - 1) {
				dstrlist := setstring(*dset, row + 1)
				perms := findperms(dstrlist)
				if isallsubsetplacefair(perms, row + 1) == false {
					return
				}
			}

			// If there are any dice left, move on to them
			if row < (DICE - 1) {

				// We're starting a new row, zero out the tally
				for pl := uint8(0); pl < DICE; pl++ {
					curtallycache[row][side][pl] = 0
				}

				search(dset, &(curtallycache[row][side]), row + 1, 0)

				return;
			} else {
				// This means we have now filled out all the dice

				dstrlist := setstring(*dset, DICE)
				dstr := strings.Join(dstrlist, ``)
				fmt.Printf("Got placefair set: %s\n", dstr)
				placefaircount++

				perms := findperms(dstrlist)

				if ispermfair(perms, DICE) {
				 	permfaircount++
				 	fmt.Printf("Got permfair set: %s\n", dstr)
				} else if isallsubsetplacefair(perms, DICE) {
				 	allsubsetplacefaircount++
				 	fmt.Printf("Got allsubsetplacefair set: %s\n", dstr)
				}


				return
			}
		}

		// Each time we start on a new row build the tally table min/max using the previous rows
		if side == 0 && row != DICE {
			find_tally_left(dset, row, tally_table, &loc_min_tally, &loc_max_tally)
		}

		// Try to prune based on the runny tally and the min/max
		for pl := uint8(0); pl < DICE; pl++ {
			// If the current running tally plus the max doesn't reach our goal
			if (*curtally)[pl] + loc_max_tally[row][side][pl] < TALLYGOAL {
				return
			}

			// If the current running tally plus the min goes over our goal
			if (*curtally)[pl] + loc_min_tally[row][side][pl] > TALLYGOAL {
				return
			}
		}

		// At this point we are still in the recursion and need to fill out
		// more columns for this row

		for d := row; d < DICE; d++ {

			// To hold the new diceset each time we change it
			// Copy current dice set to new one
			for nd := uint8(0); nd < DICE; nd++ {
				for ns := uint8(0); ns < SIDES; ns++ {
					dsetcache[row][side][nd][ns] = (*dset)[nd][ns]
				}
			}

			// Swap out the cell in this row with one below
			dsetcache[row][side][row][side] = (*dset)[d][side]
			dsetcache[row][side][d][side] = (*dset)[row][side]

			// Duplicate the running tally table to the cache
			for pl:= uint8(0); pl < DICE; pl++ {
				// Now that we've chosen a value, add it to our running tallies
				curtallycache[row][side][pl] = (*curtally)[pl] + (*tally_table)[dsetcache[row][side][row][side]][pl];
			}

			search(&(dsetcache[row][side]), &(curtallycache[row][side]), row, side + 1)

			// If this is the first column we don't permute it
			if side == 0 {
				break
			}
		} // End swapping in of lower cells in this column
	} // End search() func

	// Now call into our search function with the arguments we got
	search(&(dsetcache[row][side]), &(curtallycache[row][side]), row, side)
}
