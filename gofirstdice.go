// This whole program assumes column grouping.  Don't even *think* about trying to relax that restriction or Bad Things(TM) will happen.
// It also assumes all dice have the same number of sides.  You have been warned.

package main

import (
	"strings"
	"fmt"
	"sort"
)

const (
	DICE = int(3)
	SIDES = int(6)
)

var (
	dlet = [10]string{`a`, `b`, `c`, `d`, `e`, `f`, `g`, `h`, `i`, `j`} // 10 should be enough for anyone
)

type diceset [DICE][SIDES]int


// Sorting of permutations
type SortPerms []string
func (a SortPerms) Len() int           {return len(a)}
func (a SortPerms) Swap(i, j int)      {a[i], a[j] = a[j], a[i]}
func (a SortPerms) Less(i, j int) bool {if len(a[i]) == len(a[j]) {return a[i] < a[j]} else {return len(a[i]) < len(a[j])}}


func main() {

	var dset diceset

	for d := 0; d < DICE; d++ {
		for s := 0; s < SIDES; s++ {
			dset[d][s] = (s * DICE) + d
		}
	}

	fmt.Printf("Got str: %s\n", strings.Join(setstring(dset), ``))

	printperms(findperms(strings.Split("abcdeecdabdbaececdabbecdadbcaeacbdeecdabbeacdeabdcdacebbacdedcaebacbdeabcdeedcbaedbcabeacdedcabbecadcdbaedcaebbadceedbcaeacbdadcebbadceceabdbadceedcba", ``)))

}


func setstring(dset diceset) []string {

	dstr := make([]string, DICE * SIDES)

	for i := 0; i < (DICE * SIDES); i++ {
		s := i / DICE;
		for d := 0; d < DICE; d++ {
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
