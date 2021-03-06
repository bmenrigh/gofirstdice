// This whole program assumes column grouping.  Don't even *think* about trying to relax that restriction or Bad Things(TM) will happen.
// It also assumes all dice have the same number of sides.  You have been warned.

package main

import (
	"runtime"
	"os"
	"strings"
	"fmt"
	"sort"
	"sync"
)

const (
	DICE = uint8(4)
	SIDES = uint8(12)
	COLMASK = uint8(3) // Number of cols to permute before greating goroutine
	THREADS = uint8(8) // Number of concurrent goroutines
	PATHSIZE = uint64(5185)     // (SIDES^DICE / DICE) + 1 // 4x12
	//PATHSIZE = uint64(26245)    // (SIDES^DICE / DICE) + 1 // 4x18
	//PATHSIZE = uint64(202501)   // (SIDES^DICE / DICE) + 1 // 4x30
	//PATHSIZE = uint64(4860001)  // (SIDES^DICE / DICE) + 1 // 5x30
	//PATHSIZE = uint64(72001)  // (SIDES^DICE / DICE) + 1 // 3x60
	//PATHSIZE = uint64(3240001)  // (SIDES^DICE / DICE) + 1 // 4x60
)

var (
	PERMGOAL uint64
	TALLYGOAL uint64
	permfaircount int
	placefaircount int
	threadcount int
	allsubsetplacefaircount int
	countermutex = &sync.Mutex{}
	iomutex = &sync.Mutex{}
)

var (
	dlet = [10]string{`a`, `b`, `c`, `d`, `e`, `f`, `g`, `h`, `i`, `j`} // 10 should be enough for anyone
)

type diceset [DICE][SIDES]uint8
type placetally [DICE * SIDES][DICE]uint64 // [value] then [place]
type tallytable [DICE][SIDES][DICE]uint64   // [row] then [column] then [place]
type runningtally [DICE]uint64   // [place]
type pathtable [SIDES + 1][DICE][PATHSIZE]uint8 // [column] then [place] then [place sum number]
type balance [DICE][DICE]uint8 // [die] and then count of mod dice


// Sorting of permutations
type SortPerms []string
func (a SortPerms) Len() int           {return len(a)}
func (a SortPerms) Swap(i, j int)      {a[i], a[j] = a[j], a[i]}
func (a SortPerms) Less(i, j int) bool {if len(a[i]) == len(a[j]) {return a[i] < a[j]} else {return len(a[i]) < len(a[j])}}


func main() {

	runtime.GOMAXPROCS(runtime.NumCPU())
	fmt.Fprintf(os.Stderr, "Running on %d CPUs\n", runtime.GOMAXPROCS(0))

	PERMGOAL = intpow(SIDES, DICE) / factorial(DICE)
	TALLYGOAL = intpow(SIDES, DICE) / uint64(DICE)
	permfaircount = 0
	placefaircount = 0
	allsubsetplacefaircount = 0

	threaded_fill_table_by_row()

	//fmt.Printf("Got str: %s\n", strings.Join(setstring(dset, DICE), ``))

	//printperms(findperms(strings.Split("abcdeecdabdbaececdabbecdadbcaeacbdeecdabbeacdeabdcdacebbacdedcaebacbdeabcdeedcbaedbcabeacdedcabbecadcdbaedcaebbadceedbcaeacbdadcebbadceceabdbadceedcba", ``)))

	//isallsubsetplacefair(findperms(strings.Split("abcddcbadbcaacbdcbdaadbccbdaadbcdbcaacbdabdccdba", ``)))
	//isallsubsetplacefair(findperms(strings.Split("abcddcbadbcacabdabcddcbaadcbbcdabdacacdbadbccbda", ``)))

	//fmt.Printf("%d, %d : %d\n", 12, 3, binomial(12,3))

	fmt.Fprintf(os.Stderr, "Place fair: %d; All subset place fair: %d; Perm fair: %d\n", placefaircount, allsubsetplacefaircount, permfaircount)

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


func findbalance (dset *diceset) balance {

	var btable balance

	for d := uint8(0); d < DICE; d++ {
		for s := uint8(0); s < SIDES; s++ {
			btable[d][(*dset)[d][s] % DICE] += 1
		}
	}

	return btable
}


func printbalance (btable balance) {

	fmt.Fprintf(os.Stderr, "--------------------\n")
	for d := uint8(0); d < DICE; d++ {
		fmt.Fprintf(os.Stderr, "d%d: ", d)
		for p := uint8(0); p < DICE; p++ {
			fmt.Fprintf(os.Stderr, " | %2d", btable[d][p])
		}
		fmt.Fprintf(os.Stderr, " |\n")
	}
	fmt.Fprintf(os.Stderr, "\n")
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
		fmt.Fprintf(os.Stderr, "\"%s\" -> %d\n", p, perms[p])
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

				//fmt.Fprintf(os.Stderr, "n=%d, p=%d, c=%d\n", (s * DICE) + d, p, tally_cache[v][p]);
			}
		}
	}

	// for v := uint64(0); v < uint64(SIDES) * uint64(DICE); v++ {
	// 	for p := uint8(0); p < DICE; p++ {
	// 		fmt.Fprintf(os.Stderr, "tally v: %d; p: %d; t: %d\n", v, p, tally_cache[v][p])
	// 	}
	// }

	return tally_cache
}


func find_tally_left(dset *diceset, row uint8, tally_table *placetally,
	min_tally *tallytable, max_tally *tallytable) {

	l := int8(SIDES - 1) // The limit

	for p := uint8(0); p < DICE; p++ { // for each place
		for s := l; s >= 0; s-- { // start from last to first
			min := intpow(SIDES, DICE)
			max := uint64(0)

			for d := row; d < DICE; d++ { // for each dice you could pick
				v := (*dset)[d][s]; // The value

				t := (*tally_table)[v][p]

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
				(*min_tally)[row][s][p] = min;
				(*max_tally)[row][s][p] = max;
			} else {
				(*min_tally)[row][s][p] = min + (*min_tally)[row][s + 1][p];
				(*max_tally)[row][s][p] = max + (*max_tally)[row][s + 1][p];
			}
			// fmt.Fprintf(os.Stderr, "Bringing min,max for s=%d; p=%d to %d, %d\n", s, p, (*min_tally)[row][s - 1][p], (*max_tally)[row][s - 1][p]);

		} // end for s
	}
}


func find_tally_path(dset *diceset, tally_table *placetally, path_table *pathtable) {

	for p := uint8(0); p < DICE; p++ { // for each place
		for s := int8(SIDES); s >= 0; s-- { // start from last to first

			// Zero out all all the numbers here
			for n := uint64(0); n < PATHSIZE; n++ {
				(*path_table)[s][p][n] = 0
			}

			if (s == int8(SIDES)) {
				// The very end starts with the tally goal
				(*path_table)[s][p][TALLYGOAL] = 1
			} else {

				for d := uint8(0); d < DICE; d++ { // for each dice you could pick
					v := (*dset)[d][s]; // The value

					t := (*tally_table)[v][p] // The tally

					for n := t; n < PATHSIZE; n++ {
						// If the next row up had this number
						// we should subtract the current tally
						// to find the possible number for this row

						if (*path_table)[s + 1][p][n] == 1 {
							(*path_table)[s][p][n - t] = 1

							//fmt.Fprintf(os.Stderr, "path_table r: %d; s: %d; p: %d, n: %d\n", row, s, p, n - t);
						}
					}
				} // end for d
			} // end is last row
		} // end for s
	} // end for place
}


func threaded_fill_table_by_row() {

	var threadwait = make(chan bool, THREADS)
	waitgroup := sync.WaitGroup{}
	threadcount = 1

	// Build the dice set table
	// The whole space gets searched no matter what the permutation of the
	// initial dice set is.
	// Permuting it "nicely" probably reduces the time to the first
	// solution found
	var dset diceset
	for d := uint8(0); d < DICE; d++ {
		// Make even columns ascending
		for s := uint8(0); s < SIDES; s += 2 {
			dset[d][s] = (uint8(s) * DICE) + uint8(d)
		}
		// And odd columns descending
		for s := uint8(1); s < SIDES; s += 2 {
			dset[d][s] = (uint8(s) * DICE) + ((DICE - 1) - uint8(d))
		}
	}

	// Compute the per-number tally
	tally_table := build_tally_table()

	// Build the min/max tally
	var min_tally, max_tally tallytable
	find_tally_left(&dset, 0, &tally_table, &min_tally, &max_tally)

	// Build the tally path
	var path_table pathtable
	find_tally_path(&dset, &tally_table, &path_table)

	// Start out with zeroed tally
	var zerotally runningtally

	for pl := uint8(0); pl < DICE; pl++ {
		zerotally[pl] = 0
	}

	// This function permutes the first colmask columns and then calls
	// a goroutine to permute the rest

	var permute func(*diceset, uint8, uint8)
	permute = func(dset *diceset, row uint8, side uint8) {

		if side >= COLMASK {
			// If there are any dice left, move on to them
			if row < (DICE - 1) {

				permute(dset, row + 1, 0)

				return;
			} else {
				// We've reached the end of the masked columns
				// Now we need to do the rest of the perms for real

				// Send a message over the buffered channel
				// this will block if the channel is full and cause
				// us to pause until a goroutine takes something
				// out of the channel
				threadwait <- true

				waitgroup.Add(1)
				go func(tnum int) {

					iomutex.Lock()
				 	fmt.Fprintf(os.Stderr, "[STARTING WORKER THREAD #%d]\n", tnum)
					iomutex.Unlock()

					fill_table_by_row(dset, &zerotally, 0, 0, &tally_table, &min_tally, &max_tally, &path_table)

					<-threadwait

					waitgroup.Done()

					iomutex.Lock()
					fmt.Fprintf(os.Stderr, "[FINISHED WORKER THREAD #%d]\n", tnum)
					iomutex.Unlock()

				}(threadcount)

				threadcount++

				return
			}
		} else {
			// Okay we still have more permuatitons to do for this column

			for d := row; d < DICE; d++ {

				var newdset diceset
				// To hold the new diceset each time we change it
				// Copy current dice set to new one
				newdset = *dset

				// Swap out the cell in this row with one below
				newdset[row][side] = (*dset)[d][side]
				newdset[d][side] = (*dset)[row][side]

				permute(&newdset, row, side + 1)

				// If this is the first column, we never permute it
				if side == 0 {
					break
				}
			}
		}
	}
	// Start the permutations
	permute(&dset, 0, 0)

	waitgroup.Wait() // Wait for remaining goroutines to finish
}


func fill_table_by_row(dset *diceset, curtally *runningtally, row uint8, side uint8,
	tally_table *placetally, min_tally *tallytable, max_tally *tallytable, path_table *pathtable) {

	// Make local copies of the min/max tally tables
	loc_min_tally, loc_max_tally := *min_tally, *max_tally

	// Pre-allocate a runningtally for each recursion call so we
	// don't have to constantly make a new one and have the GC
	// constantly collecting them
	var curtallycache[DICE][SIDES + 1]runningtally

	// Copy the passed curtally into the cache
	curtallycache[row][side] = *curtally

	// Pre-allocate a dset for each recursion call too
	var dsetcache[DICE][SIDES + 1]diceset

	// Copy the passed dset into the cache
	dsetcache[row][side] = *dset

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
			if row < (DICE - 2) {

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

				countermutex.Lock()
				placefaircount++
				countermutex.Unlock()

				iomutex.Lock()
				fmt.Printf("Got placefair set: %s\n", dstr)
				iomutex.Unlock()

				perms := findperms(dstrlist)

				if ispermfair(perms, DICE) {
					countermutex.Lock()
				 	permfaircount++
					countermutex.Unlock()

					iomutex.Lock()
				 	fmt.Printf("Got permfair set: %s\n", dstr)
					//printbalance(findbalance(dset))
					iomutex.Unlock()
				} else if isallsubsetplacefair(perms, DICE) {
					countermutex.Lock()
				 	allsubsetplacefaircount++
					countermutex.Unlock()

					iomutex.Lock()
				 	fmt.Printf("Got allsubsetplacefair set: %s\n", dstr)
					iomutex.Unlock()
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
			t := (*curtally)[pl] // the current tally

			// If the current running tally plus the max doesn't reach our goal
			if t + loc_max_tally[row][side][pl] < TALLYGOAL {
				return
			}

			// If the current running tally plus the min goes over our goal
			if t + loc_min_tally[row][side][pl] > TALLYGOAL {
				return
			}

			// If there is no path from here
			if (*path_table)[side][pl][t] == 0 {
				//fmt.Printf("Pruning. Got no path for row %d; side %d; pl %d; t %d\n", row, side, pl, t);
				return
			}
		}

		// At this point we are still in the recursion and need to fill out
		// more columns for this row

		for d := row; d < DICE; d++ {

			// To hold the new diceset each time we change it
			// Copy current dice set to new one
			dsetcache[row][side] = *dset

			// Swap out the cell in this row with one below
			dsetcache[row][side][row][side] = (*dset)[d][side]
			dsetcache[row][side][d][side] = (*dset)[row][side]

			// Duplicate the running tally table to the cache
			for pl:= uint8(0); pl < DICE; pl++ {
				// Now that we've chosen a value, add it to our running tallies
				curtallycache[row][side][pl] = (*curtally)[pl] + (*tally_table)[dsetcache[row][side][row][side]][pl];
			}

			search(&(dsetcache[row][side]), &(curtallycache[row][side]), row, side + 1)

			// If this is a masked column we don't permute it
			if side < COLMASK {
				break
			}
		} // End swapping in of lower cells in this column
	} // End search() func

	// Now call into our search function with the arguments we got
	search(&(dsetcache[row][side]), &(curtallycache[row][side]), row, side)
}
