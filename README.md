"go first dice" are a set of dice for N players, where each player rolls one of N dice. The player with the highest roll goes first, the player with the second highest roll goes second, and so on.

A set of dice is said to be place-fair if every player has an equal chance of going first, second, third, an so on. That is, no die has a place advantage of any other die.

A set of dice is said to be perm-fair if every ordering (permutation) of players is equally likey. This is the strongest form of fairness.

A set of dice is said to be all-subset-place-fair if M players can grab any subset of M dice from N (where M <= N) and those M dice are place-fair. It's trivial to see that perm-fairness implies all-subset-place-fairness, but all-subset-place-fairness doesn't guaruntee perm-fairness.


Some shorthand we use for configurations of dice is that if there are 4 dice, each with 12 sides (48 total sides) then that is a 4d12 set. Ties can guarunteed not to happen if each side gets a unique number, 1 through S. So in the 4d12 set the numbers 1 - 48. If numbers are assigned to dice in pairs of lowest & highest, like 1 & 48, 2 & 47, etc. then the dice are said to have "mirror symmetry". If numbers are distributed evenly across dice in groups, like 1 - 4 distributned to the 4 dice, and then 5 - 8 distributed to the 4 dice, and so on, the dice are said to have "column grouping".

An example of a 4d12 configuration with both column grouping and mirror symmetry is:

Die:  	1 	8 	11 	14 	19 	22 	27 	30 	35 	38 	41 	48
Die:  	2 	7 	10 	15 	18 	23 	26 	31 	34 	39 	42 	47
Die:  	3 	6 	12 	13 	17 	24 	25 	32 	36 	37 	43 	46
Die:  	4 	5 	9 	16 	20 	21 	28 	29 	33 	40 	44 	45
