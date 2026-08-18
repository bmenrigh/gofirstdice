""" 7 lines :D
s = "abcddbaccbadcabddacbdbcadcbadcabbcaddacbbacdabcdacbdbcaddbacdabccabddcba"
count = {"" : 1}
for c in s:
    for k in count.keys():
        if k.count(c)==0:
            k2 = k + c
            count[k2] = count.get(k2,0) + count[k]
"""        
            
# string representing which die each number is on
s = "abcddbaccbadcabddacbdbcadcbadcabbcaddacbbacdabcdacbdbcaddbacdabccabddcba"
s = "abcddbcadbcaabcdacbdacbdadcbcbdadcbadcbadbcadcbacbdacbaddabccbaddabcadbcabcdacbdabcdabcdadbcbcdadbcadbcadcbaacbdacbddcba"
#s = "bbbbbbaaaaaaaaaaaabbbbbb"

# initialize the counts
count = {"" : 1}

# foreach number in [0,s*n) or rather, which die it's on
for c in s:
    # foreach permutation that doesn't contain c
    # these permutations are of any length
    for k in count.keys():
        if k.count(c)==0:
            #k2 = the permutation now with c on the end
            k2 = k + c
            #increment the count.  had to do .get since the perm may not exist
            count[k2] = count.get(k2,0) + count[k]
            # example for c = "a", k = "bc"
            # count["bca"] = count["bca"] + count["bc"]

# just output the perms of length 4
keys = count.keys()
keys.sort()
maxlen = max([len(key) for key in keys])
for i in range(maxlen+1):
    for k in keys:
        if len(k)==i:
            print k, count[k]

