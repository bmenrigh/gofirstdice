/*
 * To change this template, choose Tools | Templates
 * and open the template in the editor.
 */
package dice.multiside;

import java.io.BufferedReader;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.io.Reader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;

/**
 *
 * @author Landon
 */
public class WeaveMulti {

    static final int FLAG_MIRROR = 0x1;
    static final int FLAG_COLUMN = 0x2;
    static final int FLAG_TYPE2 = 0x4 | 0x2;
    //
    final int SIDES[];
    final int PLAYERS;
    final int REAL_SIDES[];
    final int TOTAL_SIDES[];
    final boolean ENFORCE_MIRRORING;
    final boolean ENFORCE_COLUMN;
    final boolean ENFORCE_TYPE2;
    final int fact[] = {1, 1, 2, 6, 24, 120, 720, 5040, 40320};
    //
    char strings2[][];// = new char[PLAYERS + 1][TOTAL_SIDES[PLAYERS]];
    int indices[][];// = new int[PLAYERS + 1][];//[SIDES[PLAYERS - 1]];
    long solutions[];// = new long[PLAYERS + 1];
    //
    List<List<String>> perms = new ArrayList<List<String>>();
    int permIndex[];// = new int[PLAYERS + 2];
    String allPerms[];
    int numAllPerms;
    int appendCounterSize;
    int appendMap[][];
    int weaveMap[][][];// = new int[PLAYERS + 1][][];
    int targetC[];// = new int[PLAYERS + 1];
    //    
    int countsBeforeIntN[][][];
    int countsAfterIntN[][][];
    int counterClear[];
    ///
    int insertCounts[][][];// = new int[PLAYERS + 1][][];
    int currentCounts[][][];// = new int[PLAYERS + 1][][];
    // [n][lastinsert][i<n!]  old
    int minCounts[][][];// = new int[PLAYERS + 1][][];
    int maxCounts[][][];// = new int[PLAYERS + 1][][];
    // [n][current s][lastinsert][i<n!] new
    int minCounts2[][][][];// = new int[PLAYERS + 1][][][];
    int maxCounts2[][][][];// = new int[PLAYERS + 1][][][];
    int minCountsSum[][][][];// = new int[PLAYERS + 1][][][];
    int maxCountsSum[][][][];// = new int[PLAYERS + 1][][][];
    // [n][s][lastindex]
    int lowerBound[][][];// = new int[PLAYERS + 1][][];
    int upperBound[][];// = new int[PLAYERS + 1][];
    // [n][low][high][i<n!]
    int minLH[][][][];// = new int[PLAYERS + 1][][][];
    int maxLH[][][][];// = new int[PLAYERS + 1][][][];
    // const caches  
    //[n][i<n!]
    int worstMin[][];// = new int[PLAYERS + 1][];
    int worstMax[][];// = new int[PLAYERS + 1][];
    ///////////
    long nextTimes[];// = new long[PLAYERS + 2];
    long minuteReport;
    //////

    public WeaveMulti(int SIDES[]) {
        this(SIDES, 0);
    }

    public WeaveMulti(int SIDES[], int flags) {
        this.SIDES = SIDES;
        PLAYERS = SIDES.length;
        REAL_SIDES = new int[PLAYERS];

        //////////////////
        TOTAL_SIDES = new int[PLAYERS + 1];
        ENFORCE_MIRRORING = (flags & FLAG_MIRROR) == FLAG_MIRROR;
        ENFORCE_COLUMN = (flags & FLAG_COLUMN) == FLAG_COLUMN;
        ENFORCE_TYPE2 = (flags & FLAG_TYPE2) == FLAG_TYPE2;
        ///
        strings2 = new char[PLAYERS + 1][TOTAL_SIDES[PLAYERS]];
        indices = new int[PLAYERS + 1][];//[SIDES[PLAYERS - 1]];
        solutions = new long[PLAYERS + 1];
        //
        permIndex = new int[PLAYERS + 2];

        weaveMap = new int[PLAYERS + 1][][];
        targetC = new int[PLAYERS + 1];
        ///
        insertCounts = new int[PLAYERS + 1][][];
        currentCounts = new int[PLAYERS + 1][][];
        // [n][lastinsert][i<n!]  old
        minCounts = new int[PLAYERS + 1][][];
        maxCounts = new int[PLAYERS + 1][][];
        // [n][current s][lastinsert][i<n!] new
        minCounts2 = new int[PLAYERS + 1][][][];
        maxCounts2 = new int[PLAYERS + 1][][][];
        minCountsSum = new int[PLAYERS + 1][][][];
        maxCountsSum = new int[PLAYERS + 1][][][];
        // [n][s][lastindex]
        lowerBound = new int[PLAYERS + 1][][];
        upperBound = new int[PLAYERS + 1][];
        // [n][low][high][i<n!]
        minLH = new int[PLAYERS + 1][][][];
        maxLH = new int[PLAYERS + 1][][][];
        // const caches  
        //[n][i<n!]
        worstMin = new int[PLAYERS + 1][];
        worstMax = new int[PLAYERS + 1][];
        ///
        nextTimes = new long[PLAYERS + 2];
        ///
        indices[0] = new int[0];
        strings2[0] = new char[0];
        for (int i = 0; i < PLAYERS; ++i) {
            TOTAL_SIDES[i + 1] = TOTAL_SIDES[i] + SIDES[i];

            REAL_SIDES[i] = SIDES[i] / (ENFORCE_MIRRORING ? 2 : 1);

            indices[i + 1] = new int[SIDES[i]];
            strings2[i + 1] = new char[TOTAL_SIDES[i + 1]];
        }



        /////////////////////////////////////////////


        //int permsize = 1;
        int permIndex[] = new int[PLAYERS + 2];
        permIndex[1] = 1;

        {
            List<String> t = new ArrayList<String>();
            t.add("");
            perms.add(t);

            for (int i = 0; i < PLAYERS; ++i) {
                t = perms.get(i);
                List<String> t2 = new ArrayList<String>();
                for (String s : t) {
                    for (int j = 0; j < PLAYERS; ++j) {
                        char c = (char) ('a' + j);
                        if (s.indexOf(c) == -1) {
                            String s2 = s + c;
                            t2.add(s2);
                        }
                    }
                }
                permIndex[i + 2] = permIndex[i + 1] + t2.size();
                //System.out.print(t2.size() + " ");
                //System.out.println(t2);
                perms.add(t2);
            }
            //System.out.print("        - ");
            //System.out.println(Arrays.toString(permIndex));

            numAllPerms = permIndex[PLAYERS + 1];
            allPerms = new String[numAllPerms];
            //sort
            for (int i = 0; i <= PLAYERS; ++i) {
                t = perms.get(i);
                String[] a = new String[t.size()];
                t.toArray(a);
                System.arraycopy(a, 0,
                        allPerms, permIndex[i],
                        t.size());

                for (int j = 0; j < t.size(); ++j) {
                    for (int k = 0; k < t.size() - 1; ++k) {
                        String s0 = t.get(k);
                        String s1 = t.get(k + 1);
                        if (bigChar(s0) > bigChar(s1)) {
                            t.set(k, s1);
                            t.set(k + 1, s0);
                        }
                    }
                }
                //System.out.print(t.size() + " ");
                //System.out.println(t);
            }
            //System.out.println();
            //System.out.println(Arrays.toString(permIndex));
            //System.out.println(Arrays.toString(allPerms));
            //System.out.println(numAllPerms);

            //build map
            appendCounterSize = numAllPerms + 1;
            appendMap = new int[numAllPerms + 1][PLAYERS];

            for (int i = 0; i < numAllPerms; ++i) {
                for (int j = 0; j < PLAYERS; ++j) {
                    String s = allPerms[i] + (char) ('a' + j);
                    int k;
                    for (k = 0; k < numAllPerms; ++k) {
                        if (allPerms[k].equals(s)) {
                            break;
                        }
                    }
                    appendMap[i][j] = k;
                }
            }
            //System.out.println(Arrays.toString(appendMap[0]));
            //System.out.println(Arrays.toString(appendMap[1]));
            /*
            printA(appendMap[6]);
            printA(appendMap[26]);
            printA(appendMap[86]);
            printA(appendMap[206]);
             */

            //weave mapper
            // weaveMap[n][s][0] = a;
            // weaveMap[n][s][1] = b;
            // a+ n +b = s;
            for (int n = 0; n <= PLAYERS; ++n) {
                int size = perms.get(n).size();
                //System.out.println(n + " " + size);
                weaveMap[n] = new int[size][2];
                char c = (char) ('a' + n - 1);
                for (int a = 0; a < permIndex[n]; ++a) {
                    for (int b = 0; b < permIndex[n]; ++b) {
                        String s = allPerms[b] + c + reverse(allPerms[a]);
                        int si = perms.get(n).indexOf(s);
                        if (si != -1) {
                            //System.out.println(allPerms[a] + "-" + c + "-" + allPerms[b]);
                            weaveMap[n][si][0] = b;
                            weaveMap[n][si][1] = a;
                        }
                    }
                }
                //System.out.print("weave ");
                //printA(weaveMap[n]);

                //System.out.println("weave " + n + " " + size);
                for (int si = 0; si < perms.get(n).size(); ++si) {
                    //System.out.print(String.format("%3d ", weaveMap[n][si][0]));
                }
                //System.out.println();
                for (int si = 0; si < perms.get(n).size(); ++si) {
                    //System.out.print(String.format("%3d ", weaveMap[n][si][1]));
                }
                //System.out.println();
            }



            //System.exit(0);
        }

        /**/
        for (int n = 1; n <= PLAYERS; ++n) {
            int insertPoints = TOTAL_SIDES[n] + 1;
            int halfPoints = insertPoints / 2 + 1;
            insertCounts[n] = new int[insertPoints][fact[n]];
            currentCounts[n] = new int[SIDES[n - 1] + 1][fact[n]];
            minCounts[n] = new int[insertPoints][fact[n]];
            maxCounts[n] = new int[insertPoints][fact[n]];

            minCounts2[n] = new int[SIDES[n - 1] + 1][insertPoints][fact[n]];
            maxCounts2[n] = new int[SIDES[n - 1] + 1][insertPoints][fact[n]];
            minCountsSum[n] = new int[SIDES[n - 1] + 1][insertPoints][fact[n]];
            maxCountsSum[n] = new int[SIDES[n - 1] + 1][insertPoints][fact[n]];

            minLH[n] = new int[insertPoints][insertPoints][fact[n]];
            maxLH[n] = new int[insertPoints][insertPoints][fact[n]];

            lowerBound[n] = new int[SIDES[n - 1]][insertPoints];
            upperBound[n] = new int[SIDES[n - 1]];

            for (int s = 0; s < SIDES[n - 1]; ++s) {
                int min, max;
                {
                    min = 0;
                    max = TOTAL_SIDES[n - 1];
                }
                if (ENFORCE_MIRRORING) {
                    max /= 2;
                }
                if (ENFORCE_COLUMN) {
                    min = s * TOTAL_SIDES[n - 1] / SIDES[n - 1];
                    max = (s + 1) * TOTAL_SIDES[n - 1] / SIDES[n - 1];
                }


                lowerBound[n][s][0] = min;
                upperBound[n][s] = max;
                for (int i = 1; i < insertPoints; ++i) {
                    //lowerBound[n][s][i] = Math.max(i, lowerBound[n][s][i - 1]);
                    lowerBound[n][s][i] = Math.max(i, min);
                }
            }
            System.out.println("upper " + Arrays.toString(upperBound[n]));


            //System.out.println("bounds n=" + n);
            for (int s = 0; s < SIDES[n - 1]; ++s) {
                //System.out.print(lowerBound[n][s][0] + " ");
            }
            //System.out.println();

            //System.out.println(Arrays.toString(upperBound[n]));
        }
        //System.exit(-1);

        for (int n = 0, product = 1; n <= PLAYERS; ++n) {
            targetC[n] = product / fact[n];
            worstMin[n] = new int[fact[n]];
            worstMax[n] = new int[fact[n]];
            for (int i = 0; i < fact[n]; ++i) {
                worstMin[n][i] = -targetC[n];
                worstMax[n][i] = targetC[n];
            }
            if (n < PLAYERS) {
                product *= SIDES[n];
            }
        }
        /**/



        countsBeforeIntN = new int[PLAYERS][][];
        countsAfterIntN = new int[PLAYERS][][];
        counterClear = new int[appendCounterSize];
        for (int n = 1; n <= PLAYERS; ++n) {
            int insertPoints = TOTAL_SIDES[n - 1] + 1;
            countsBeforeIntN[n - 1] = new int[insertPoints][appendCounterSize];
            countsAfterIntN[n - 1] = new int[insertPoints][appendCounterSize];
        }

    }

    public void depthS(int n, int s) {
        //System.out.println("n:" + n + "  s:" + s);

        if (s > 0 && s <= REAL_SIDES[n - 1]) {
            //do counts
            int lastIndex = indices[n][s - 1];
            //lastIndex = lowerBound[n][s][lastIndex];
            int insertsLeft = REAL_SIDES[n - 1] - s;

            for (int i = 0; i < fact[n]; ++i) {
                currentCounts[n][s][i] = currentCounts[n][s - 1][i] + insertCounts[n][lastIndex][i];

                if (currentCounts[n][s][i] + minCountsSum[n][s][lastIndex][i] > targetC[n]) {
                    return;
                }
                if (currentCounts[n][s][i] + maxCountsSum[n][s][lastIndex][i] < targetC[n]) {
                    return;
                }
            }
        }





        if (s == REAL_SIDES[n - 1]) {
            //mirror the indices if needed
            if (ENFORCE_MIRRORING) {
                for (int i = 0; i < s; ++i) {
                    indices[n][SIDES[n - 1] - i - 1] = TOTAL_SIDES[n - 1] - indices[n][i];
                }
            }

            char c = (char) ('a' + n - 1);

            for (int i = 0, j = 0; j < SIDES[n - 1] || i < TOTAL_SIDES[n - 1];) {
                if (j < SIDES[n - 1] && indices[n][j] == i) {
                    strings2[n][i + j] = c;
                    ++j;
                } else {
                    strings2[n][i + j] = strings2[n - 1][i];
                    ++i;
                }
            }

            //next depth
            depthN(n + 1);
        } else {
            int min = lowerBound[n][s][s == 0 ? 0 : indices[n][s - 1]];
            int max = upperBound[n][s];

            if (ENFORCE_TYPE2 && s % 2 == 1 && n > 1) {
                int a = lowerBound[n][s - 1][0];
                int b = upperBound[n][s];
                int c = a + b;
                int d = c - indices[n][s - 1];

                System.out.println(d);

                min = max = d;
            }

            //if(n>1 && s==0 )
            if (n > 1 && s == 0 && SIDES[n - 1] == SIDES[n - 2]) {
                max = indices[n - 1][0];
            }

            for (int i = min; i <= max; ++i) {
                indices[n][s] = i;
                depthS(n, s + 1);
            }
        }
    }

    public void report(int n, PrintStream o) {
        o.print("(" + smallestCond + ") ");
        o.print(n + " ");
        o.print(solutions[n - 1]);
        o.print(" ");
        o.print(strings2[n - 1]);
        o.print(" ");
        o.print(Arrays.toString(solutions));
        o.println();

    }

    public void depthN(int n) {
        ++solutions[n - 1];

        long nt = System.nanoTime();
        /**/
        //if (nextTimes[n] < nt) { // 10,000
        if (nextTimes[n] < nt && solutions[n - 1] % 1000 == 0) { // 10,000
            //if (nextTimes[n] < nt && (solutions[n - 1] & 1023) == 0) { // 10,000
            System.out.print(String.format("%.4f ", (nt - nextTimes[n]) / 1000000000.) + " ");
            nextTimes[n] = nt + 1000000000l; //only do 1 per second
            minuteReport = nt + 1000000000l * 60; //only once per minute
            report(n, System.out);
        }
        /**/
        if (minuteReport < nt) {
            minuteReport = nt + 1000000000l * 60; //only once per minute

            System.out.print(new Date());
            System.out.print(" ");
            System.out.print(Arrays.toString(solutions));
            /**
            System.out.print(" - " + scount + " - ");
            printA(indices[2], false);
            System.out.print(" - n=" + (n - 1) + " - ");
            printA(indices[n - 1], false);
            /**/
            System.out.println();
        }
        /**/



        if (n > PLAYERS) {
            //DONE
            if (PLAYERS >= 5) {
                //print any 5 dice sets
                /**/
                for (int i = 0; i <= PLAYERS; ++i) {
                    System.out.print(Arrays.toString(indices[i]));
                }
                System.out.println(strings2[n - 1]);
                /**/
            }
            //condense(); //for seeing fewest total sides
            return;
        }

        //String s = strings[n - 1];
        char s2[] = strings2[n - 1];

        int slen = TOTAL_SIDES[n - 1];
        int slenmin1 = slen - 1;
        int insertPoints = TOTAL_SIDES[n - 1] + 1;
        int halfPoints = insertPoints / 2 + 1;

        int countsBeforeInt[][];
        int countsAfterInt[][];
        if (!true) {
            //40.429301273
            //40.813186695
            //40.450819226
            countsBeforeInt = new int[insertPoints][appendCounterSize];
            countsAfterInt = new int[insertPoints][appendCounterSize];
        } else {
            countsBeforeInt = countsBeforeIntN[n - 1];
            countsAfterInt = countsAfterIntN[n - 1];
            for (int i = 0; i < insertPoints; ++i) {
                if (true) {
                    //40.611455451
                    //39.903771666
                    //40.626117467
                    System.arraycopy(counterClear, 0, countsBeforeInt[i], 0, appendCounterSize);
                    System.arraycopy(counterClear, 0, countsAfterInt[i], 0, appendCounterSize);
                } else {
                    //40.534913373
                    //40.324678006
                    //40.779066382
                    Arrays.fill(countsBeforeInt[i], 0);
                    Arrays.fill(countsAfterInt[i], 0);
                }
            }
        }
        countsBeforeInt[0][0] = 1;
        countsAfterInt[0][0] = 1;


        //do the before and after counts
        for (int i = 0; i < slen; ++i) {
            //int cb = (s.charAt(i) - 'a');
            //int ca = (s.charAt(s.length() - i - 1) - 'a');
            int cb = (s2[i] - 'a');
            int ca = (s2[slenmin1 - i] - 'a');
            for (int j = 0; j < numAllPerms; ++j) {
                countsBeforeInt[i + 1][j] += countsBeforeInt[i][j];
                countsBeforeInt[i + 1][appendMap[j][cb]] += countsBeforeInt[i][j];
                countsAfterInt[i + 1][j] += countsAfterInt[i][j];
                countsAfterInt[i + 1][appendMap[j][ca]] += countsAfterInt[i][j];
            }
        }

        //merge the 2 count
        for (int i = 0; i < insertPoints; ++i) {
            for (int j = 0; j < fact[n]; ++j) {
                int cb = countsBeforeInt[i][weaveMap[n][j][0]];
                int ca = countsAfterInt[insertPoints - i - 1][weaveMap[n][j][1]];
                insertCounts[n][i][j] = cb * ca;
            }
        }

        //if we enforce mirroring, load all the counts to the first half
        if (ENFORCE_MIRRORING) {
            //set the data to include it's mirror
            for (int i = 0; i < halfPoints; ++i) {
                int ii = insertPoints - i - 1;
                for (int j = 0; j < fact[n]; ++j) {
                    insertCounts[n][i][j] += insertCounts[n][ii][j];
                }
            }

            // zero the data of the mirrors
            for (int i = halfPoints; i < insertPoints; ++i) {
                for (int j = 0; j < fact[n]; ++j) {
                    insertCounts[n][i][j] = 0;
                }
            }
        }


        //calc the min/max
        {
            for (int currentS = REAL_SIDES[n - 1] - 1; currentS >= 0; --currentS) {
                for (int lastIndex = 0; lastIndex < insertPoints; ++lastIndex) {
                    int min = lowerBound[n][currentS][lastIndex];
                    int max = upperBound[n][currentS];
                    if (lastIndex > 0 && min == lowerBound[n][currentS][lastIndex - 1]
                            || lastIndex > max) {
                        minCounts2[n][currentS][lastIndex] = minCounts2[n][currentS][lastIndex - 1];
                        maxCounts2[n][currentS][lastIndex] = maxCounts2[n][currentS][lastIndex - 1];
                        minCountsSum[n][currentS][lastIndex] = minCountsSum[n][currentS][lastIndex - 1];
                        maxCountsSum[n][currentS][lastIndex] = maxCountsSum[n][currentS][lastIndex - 1];
                        continue;
                    }
                    for (int i = 0; i < fact[n]; ++i) {
                        minCounts2[n][currentS][lastIndex][i] = 1000000;
                        maxCounts2[n][currentS][lastIndex][i] = -1000000;

                        for (int j = min; j <= max; ++j) {
                            if (minCounts2[n][currentS][lastIndex][i] > insertCounts[n][j][i]) {
                                minCounts2[n][currentS][lastIndex][i] = insertCounts[n][j][i];
                            }
                            if (maxCounts2[n][currentS][lastIndex][i] < insertCounts[n][j][i]) {
                                maxCounts2[n][currentS][lastIndex][i] = insertCounts[n][j][i];
                            }
                        }
                        if (currentS > 0) {
                            minCountsSum[n][currentS][lastIndex][i] =
                                    minCountsSum[n][currentS + 1][lastIndex][i]
                                    + minCounts2[n][currentS][lastIndex][i];
                            maxCountsSum[n][currentS][lastIndex][i] =
                                    maxCountsSum[n][currentS + 1][lastIndex][i]
                                    + maxCounts2[n][currentS][lastIndex][i];
                        }
                    }
                }
            }
        }






        depthS(n, 0);
    }
    int smallestCond = Integer.MAX_VALUE;

    public String condense() {
        String out = "";
        for (char c : strings2[PLAYERS]) {
            if (out.length() == 0 || out.charAt(out.length() - 1) != c) {
                out += c;
            }
        }
        if (smallestCond > out.length()) {
            smallestCond = out.length();
            System.out.println(out.length() + " - " + new String(strings2[PLAYERS]) + " - " + out);
        }
        return out;
    }

    public long[] search() {
        long start = System.nanoTime();

        System.out.println("----------------------");

        System.out.println("Sides: " + Arrays.toString(SIDES));
        System.out.println("Mirroring: " + ENFORCE_MIRRORING);
        System.out.println("Column Grouping: " + ENFORCE_COLUMN);
        System.out.println("Type2: " + ENFORCE_TYPE2);

        depthN(1);


        double dt = (System.nanoTime() - start) / 1000000000.;

        System.out.print("Search Complete - ");
        System.out.print(Arrays.toString(solutions));
        System.out.print("- " + dt + " sec - ");

        System.out.println();

        System.out.println("Sides: " + Arrays.toString(SIDES));
        System.out.println("Mirroring: " + ENFORCE_MIRRORING);
        System.out.println("Column Grouping: " + ENFORCE_COLUMN);
        System.out.println("Type2: " + ENFORCE_TYPE2);
        System.out.println("Example: " + new String(strings2[PLAYERS]));

        return solutions;
    }

    public long[] search(String file) {
        long start = System.nanoTime();

        System.out.println("----------------------");

        System.out.println("Sides: " + Arrays.toString(SIDES));
        System.out.println("Mirroring: " + ENFORCE_MIRRORING);
        System.out.println("Column Grouping: " + ENFORCE_COLUMN);
        System.out.println("Type2: " + ENFORCE_TYPE2);


        try {
            FileInputStream fis = new FileInputStream(file);
            InputStreamReader in = new InputStreamReader(fis);
            BufferedReader br = new BufferedReader(in);
            String line = null;
            while ((line = br.readLine()) != null) {

                //System.out.println(line);
                /**
                line = line.replaceAll("a", "aa");
                line = line.replaceAll("b", "bb");
                line = line.replaceAll("c", "cc");
                line = line.replaceAll("d", "dd");
                /**/
                line = line + line;
                //System.out.println(line);
                strings2[4] = line.toCharArray();

                depthN(5);
            }
        } catch (FileNotFoundException ex) {
            ex.printStackTrace();
        } catch (IOException ex) {
            ex.printStackTrace();
        }

        double dt = (System.nanoTime() - start) / 1000000000.;

        System.out.print("Search Complete - ");
        System.out.print(Arrays.toString(solutions));
        System.out.print("- " + dt + " sec - ");

        System.out.println();

        System.out.println("Sides: " + Arrays.toString(SIDES));
        System.out.println("Mirroring: " + ENFORCE_MIRRORING);
        System.out.println("Column Grouping: " + ENFORCE_COLUMN);
        System.out.println("Type2: " + ENFORCE_TYPE2);
        System.out.println("Example: " + new String(strings2[PLAYERS]));

        return solutions;
    }

    public int bigChar(String s) {
        int c = 0;
        for (int i = 0; i < s.length(); ++i) {
            c = Math.max(c, s.charAt(i) - 'a');
        }
        return c;
    }

    public String reverse(String s) {
        String o = "";
        for (int i = 0; i < s.length(); ++i) {
            o = s.charAt(i) + o;
        }
        return o;
    }

    public static void main(String[] args) {
        if (args.length > 0) {
            String last = args[args.length - 1].toLowerCase();
            if (last.equals("m") || last.equals("mirror")) {
                int[] sides = new int[args.length - 1];
                for (int i = 0; i < args.length - 1; ++i) {
                    sides[i] = Integer.valueOf(args[i]);
                }
                new WeaveMulti(sides, FLAG_MIRROR).search();
            } else {
                int[] sides = new int[args.length];
                for (int i = 0; i < args.length; ++i) {
                    sides[i] = Integer.valueOf(args[i]);
                }
                new WeaveMulti(sides, 0).search();
            }
        } else if (false) {
            //int[] sides = {12,12,12,12, 30};
            int[] sides = {24, 24, 24, 24, 30};
            new WeaveMulti(sides, FLAG_MIRROR).search("n4s12.txt");
        } else {
            long start = System.nanoTime();
            System.out.println("doing this one  801328");
            //int[] sides = {12, 12, 12, 12};
            //int[] sides = {24, 24, 24, 24};
            //int[] sides = {30, 30, 30, 30};
            //int[] sides = {30, 30, 30, 30};
            //int[] sides = {60,60,60,60,60,60};
            int[] sides = {18, 18, 18, 18};
            //int[] sides = {6, 8, 12, 12, 12};
            //int[] sides = {54,54};
            new WeaveMulti(sides, FLAG_MIRROR | FLAG_COLUMN).search();
            //System.out.println("Time: " + (System.nanoTime() - start) / 1000000000.);
            System.out.println("----------------------");
            //new WeaveMulti(sides, false).search();
        }
    }

    @Override
    public String toString() {
        return Arrays.toString(SIDES) + " "
                + ENFORCE_MIRRORING + " "
                + Arrays.toString(solutions);
    }
}
