#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <locale.h>

#include "rc4_16.h"
#include "pavl.h"

#define DICE  5
#define SIDES 30
#define DICEMASK 7
#define SIDESMASK 31
#define GOALTALLY 4860000.0 /* SIDES^DICE / DICE */
#define CLOSEFACTOR 0.0005
#define CLOSESCORE (((GOALTALLY * CLOSEFACTOR) * \
		     (GOALTALLY * CLOSEFACTOR)) * (DICE * DICE))


#define POP_SIZE 1024 /* Must be power-of-two */
#define POP_MASK (POP_SIZE - 1)

int grid[DICE][SIDES];
int g_cache[SIDES][DICE][SIDES];

int selection[DICE];
int tallies[DICE][DICE];

struct datum {
  double score;
  int grid[DICE][SIDES];
};

struct datum population[POP_SIZE];
double worst_score;
double worst_idx;
double best_score;
struct pavl_table *pop_tree;


void distribute(int (*)[DICE][SIDES], int);
void print_grid(int (*)[DICE][SIDES]);
int next_perm(int (*)[DICE][SIDES], int (*)[DICE + 1], int, int);
int is_fair(int (*)[DICE][SIDES], int);
void print_tallies(int (*)[DICE][DICE]);
void count_perms(int (*)[DICE][SIDES], int, int (*)[DICE],
		 int (*)[DICE][DICE], int);
int check_tallies(int (*)[DICE][DICE], int);
double score_tallies(int (*)[DICE][DICE], double *);
int check_rows(int (*)[DICE][SIDES], int, int);
int check_pair_fair(int (*)[DICE][SIDES], int, int);
void fill_columns2(int (*)[DICE][SIDES], int);
void fill_columns4(int (*)[DICE][SIDES], int);
int getnum(int, int);
int add_pop(struct datum *, int);
double worst_pop_score();
void climb();
void mutate(int (*)[DICE][SIDES]);
void mutate2(int (*)[DICE][SIDES]);
int compare_datum(const void *, const void *,
		  __attribute__((__unused__)) void *);
int insert_or_find_datum(struct datum *);
void delete_datum(struct datum *);


int main(void) {

  int i, j;
  double linear;

  struct datum d;

  /* use unitialized stack memory */
  uint16_t rand_key[1000];
  rc4_16_init(&rand_key, 1000);

  /*for (i = 0; i < 100; i++) {
    printf("%d\n", getnum(SIDES, SIDESMASK));
    }*/

  for (j = 0; j < DICE; j++) {
    for (i = 0; i < SIDES; i++) {
      grid[j][i] = (i * DICE) + j;
    }
  }
  /*print_grid(&grid); */

  /* Setup this datum */
  memcpy(&(d.grid), &grid, sizeof(grid));

  memset(&tallies, 0, sizeof(tallies));
  count_perms(&grid, 0, &selection, &tallies, DICE);
  d.score = score_tallies(&tallies, &linear);

  /* Setup the population array */
  for (i = 0; i < POP_SIZE; i++) {
    memcpy(&(population[i]), &d, sizeof(struct datum));
  }
  worst_score = worst_pop_score();
  best_score = worst_score;

  /* Setup the population tree */
  pop_tree = pavl_create(compare_datum, NULL, NULL);
  insert_or_find_datum(&d);

  /*distribute(&grid, 0);*/

  climb();


  return 0;
}

void climb() {

  int rp, rm, i;
  double new_worst, linear;
  struct datum d;
  int last_good;
  int mut_limit;
  int am_close = 0;

  char *locale;

  locale = setlocale(LC_NUMERIC, "en_US.iso88591");
  if (locale == NULL) {
    return;
  }
  
  fprintf(stderr, "The close factor (%f) score is %'ld\n", CLOSEFACTOR,
	  (long)CLOSESCORE);


  last_good = 0;
  mut_limit = ((SIDES * DICE) / 2);
  while (1 == 1) {
    
    if (last_good > (1024 * POP_SIZE)) {
      mut_limit = ((SIDES * DICE) / 4);
    }
    else {
      mut_limit = 8;
    }

    rp = getnum(POP_SIZE, POP_MASK);
    
    memcpy(&d, &(population[rp]), sizeof(struct datum));
    
    for (i = 0; i < mut_limit; i++) {
      /* Do a random mutation */
      rm = getnum(2, 1);

      if (rm == 0) {
	mutate(&(d.grid));
      }
      else {
	mutate2(&(d.grid));
      }

      /* Do the rows all look fairish? */
      if (check_rows(&(d.grid), SIDES, 0) != 1) {
	continue;
      }

      if (am_close == 1) {
	/* Check some pairwise fairness */
	if (check_pair_fair(&(d.grid), 0, 1) != 1) {
	  continue;
	}
	if (check_pair_fair(&(d.grid), 0, 2) != 1) {
	  continue;
	}
	if (check_pair_fair(&(d.grid), 0, 3) != 1) {
	  continue;
	}
	if (check_pair_fair(&(d.grid), 1, 2) != 1) {
	  continue;
	}
	if (check_pair_fair(&(d.grid), 1, 3) != 1) {
	  continue;
	}
	if (check_pair_fair(&(d.grid), 2, 3) != 1) {
	  continue;
	}
	if (DICE > 4) {
	  if (check_pair_fair(&(d.grid), 0, 4) != 1) {
	    continue;
	  }
	  if (check_pair_fair(&(d.grid), 1, 4) != 1) {
	    continue;
	  }
	  if (check_pair_fair(&(d.grid), 2, 4) != 1) {
	    continue;
	  }
	  if (check_pair_fair(&(d.grid), 3, 4) != 1) {
	    continue;
	  }
	}
      }
      
      memset(&tallies, 0, sizeof(tallies));
      count_perms(&(d.grid), 0, &selection, &tallies, DICE);
      d.score = score_tallies(&tallies, &linear);
      
      if (d.score == 0) {
	printf("Got set that is fair for all places\n");
	print_grid(&(d.grid));
	print_tallies(&tallies);
      }
      
      if (d.score < worst_score) {
	
	/*fprintf(stderr, "Adding with %d mutations\n", i);*/
	if (add_pop(&d, worst_idx) == 0) {
	  /*print_grid(&(d.grid)); */
	  continue;
	}
	last_good = 0;
	
	new_worst = worst_pop_score();

	if ((am_close == 0) && (worst_score < CLOSESCORE)) {
	  fprintf(stderr, "Worst close_score (%f) dropped below threshold\n",
		  worst_score);
	  am_close = 1;
	}

	if (d.score < best_score) {
	  best_score = d.score;
	  fprintf(stderr, "\nNew best score: %'ld (worst: %'ld)\n", (long)best_score,
		  (long)worst_score);
	  print_tallies(&tallies);
	}
	if (new_worst < worst_score) {
	  worst_score = new_worst;
	  /*fprintf(stderr, "New worst score: %0.3f\n", worst_score);*/
	}
      }
    }
    
    
    last_good++;
  }

  setlocale(LC_NUMERIC, locale);
}


void distribute(int (*g)[DICE][SIDES], int depth) {

  int k;
  int p[DICE + 1];

  /*fprintf(stderr, "depth: %d\n", depth); */

  /*if (depth == 2) {
    print_grid(g);
    }*/

  if ((depth >= 0) && (depth % 2 == 0)) {
    if (check_rows(g, depth, 2) == 0) {
      return;
    }
  }

  /*
  if ((depth == 6) || (depth == 10)) {
    if (check_rows(g, depth, 1) == 0) {
      return;
    }
  }

  if ((depth == 4) || (depth == 8) || (depth == 12)) {
    if (check_rows(g, depth, 0) == 0) {
      return;
    }
    }*/

  /* see if we've solved it */
  /*if (depth >= (SIDES)) {*/
  if (depth >= (SIDES / 2)) {
    if (check_rows(g, SIDES, 0) == 1) {
      /*if (is_fair(g, SIDES) == 1) { */
      /*printf("Got preliminary fair:\n");
	print_grid(g);*/
      
      /* Just check the first two, first */
      /*memset(&tallies, 0, sizeof(tallies));
      count_perms(g, 0, &selection, &tallies, 2);
      if (check_tallies(&tallies, 2) == 0) {
	return;
	}*/

      /*fprintf(stderr, "Got to end\n");*/


      if (check_pair_fair(g, 0, 1) != 1) {
	return;
      }
      if (check_pair_fair(g, 0, 2) != 1) {
	return;
      }
      if (check_pair_fair(g, 1, 2) != 1) {
	return;
      }/*
      if (check_pair_fair(g, 2, 3) != 1) {
	return;
	}*/

      /* check 3 when we're going for 4 or more */
      if (DICE >= 4) {
	memset(&tallies, 0, sizeof(tallies));
	count_perms(g, 0, &selection, &tallies, 3);
	if (check_tallies(&tallies, 3) != 3) {
	  return;
	}
      }

      /* check 4 when we're going for 5 or more */
      if (DICE >= 5) {
	memset(&tallies, 0, sizeof(tallies));
	count_perms(g, 0, &selection, &tallies, 4);
	if (check_tallies(&tallies, 4) != 4) {
	  return;
	}
      }

      /* Check to see if these are fair for first place */
      memset(&tallies, 0, sizeof(tallies));
      count_perms(g, 0, &selection, &tallies, DICE);
      if (check_tallies(&tallies, DICE) > 0) {
	printf("Got set that is fair for first place\n");
	print_grid(g);
	print_tallies(&tallies);
	/*} */
      }

      memset(&tallies, 0, sizeof(tallies));
      count_perms(g, 0, &selection, &tallies, DICE);
      if (check_tallies(&tallies, DICE) == DICE) {
	printf("Got set that is fair for all places\n");
	print_grid(g);
	/*print_tallies(&tallies);*/
	/*} */
	return;
      }

      /*fprintf(stderr, "Almost fair\n");
	print_tallies(&tallies);*/
    }
    return;
  }

  /* If we've placed an even number of colums check for fairness */
  /*if ((depth > 0) && ((depth % 2) == 0)) {
    if (is_fair(g, depth) != 1) {
      return;
    }
    }*/

  /* save this grid */
  memcpy(&(g_cache[depth]), g, sizeof(grid));

  /* init permutation */
  for (k = 0; k <= DICE; k++) {
    p[k] = k;
  }
  k = 1;

  /* run permutation */

  /*fill_columns4(&(g_cache[depth]), depth);
    distribute(&(g_cache[depth]), depth + 2);*/
  fill_columns2(&(g_cache[depth]), depth);
  distribute(&(g_cache[depth]), depth + 1);
  if (depth > 0) {
    while ((k = next_perm(&(g_cache[depth]), &p, k, depth)) != DICE) {
      /*fill_columns4(&(g_cache[depth]), depth);
	distribute(&(g_cache[depth]), depth + 2);*/
      fill_columns2(&(g_cache[depth]), depth);
      distribute(&(g_cache[depth]), depth + 1);
    }
    /*fill_columns4(&(g_cache[depth]), depth);
      distribute(&(g_cache[depth]), depth + 2);*/
    fill_columns2(&(g_cache[depth]), depth);
    distribute(&(g_cache[depth]), depth + 1);
  }


}


void fill_columns2(int (*g)[DICE][SIDES], int depth) {

  int i;

  for (i = 0; i < DICE; i++) {

    /* Set the mirror column */
    (*g)[i][SIDES - (depth + 1)] =
      (DICE * SIDES) - ((*g)[i][depth] + 1);
  }
}


void fill_columns4(int (*g)[DICE][SIDES], int depth) {

  int i;

  for (i = 0; i < DICE; i++) {

    /* Set the mirror column */
    (*g)[i][SIDES - (depth + 1)] =
      (DICE * SIDES) - ((*g)[i][depth] + 1);
 
    /* Set the next column too */
    (*g)[i][depth + 1] = ((DICE * (depth + 2)) - 1) -
      ((*g)[i][depth] % DICE);

    if ((depth + 1) != (SIDES / 2)) {
      /* Set the next's mirror column too */
      (*g)[i][SIDES - (depth + 2)] =
	(DICE * SIDES) - ((*g)[i][depth + 1] + 1);
    }
  }

}


void count_perms(int (*g)[DICE][SIDES], int depth,
		 int (*s)[DICE], int (*t)[DICE][DICE], int d) {

  int i, i2, count;

  if (depth == d) {
    for (i = 0; i < d; i++) {
      count = 0;

      for (i2 = 0; i2 < d; i2++) {
	if ((*s)[i] < (*s)[i2]) {
	  count++;
	}
      }
      (*t)[i][count] += 1;
    }

    return;
  }

  for (i = 0; i < SIDES; i++) {
    (*s)[depth] = (*g)[depth][i];
    count_perms(g, depth + 1, s, t, d);
  }

}


int is_fair(int (*g)[DICE][SIDES], int d) {

  int i, j;

  /* add cells the N and N + 1 (where N is even) colums.  to be fair
   * they must be equal to (DICE - 1) when modded by DICE
   */
  for (i = 0; (((i + 1) < d) && ((i + 1) < SIDES)); i += 2) {
    for (j = 0; j < DICE; j++) {
      if ((((*g)[j][i] + (*g)[j][i + 1]) % DICE) != DICE - 1) {
	return 0;
      }
    }
  }

  return 1;
}


int check_pair_fair(int (*g)[DICE][SIDES], int d1, int d2) {

  int i, c;

  c = 0;
  for (i = 0; i < SIDES; i++) {
    if ((*g)[d1][i] > (*g)[d2][i]) {
      c += 1;
    }
  }

  if (c == (SIDES / 2)) {
    return 1;
  }

  return 0;
}


void print_grid(int (*g)[DICE][SIDES]) {

  int i, j;

  printf("----------------------\n");

  for (j = 0; j < DICE; j++) {
    for (i = 0; i < SIDES; i++) {
      printf("%-2d ", (*g)[j][i]);
    }
    printf("\n");
  }

}


int check_tallies(int (*t)[DICE][DICE], int d) {

  int i, j, p;

  p = 0;
  for (i = 0; i < d; i++) {
    for (j = 0; j < d; j++) {

      if ((*t)[j][i] != (*t)[0][0]) {
	return p;
      }
    }
    p += 1;
  }
 
  return p;
}


double score_tallies(int (*t)[DICE][DICE], double *linear) {

  int i, j;
  double s, d;

  s = 0;
  *linear = 0;
  for (i = 0; i < DICE; i++) {
    for (j = 0; j < DICE; j++) {
      
      d = ((double)((*t)[j][i] - GOALTALLY));

      if (d < 0) {
	d *= -1;
      }

      s += (d * d);
      *linear += d;
	   
    }
  }
 
  return s;
}


void print_tallies(int (*t)[DICE][DICE]) {

  int i, j;

  printf("----------------------\n");

  for (j = 0; j < DICE; j++) {
    for (i = 0; i < DICE; i++) {
      printf("%-5d ", (*t)[j][i]);
    }
    printf("\n");
  }

}


int check_rows(int (*g)[DICE][SIDES], int d, int v) {

  int i, j, s;

  for (j = 0; j < DICE; j++) {
    s = 0;
    for (i = 0; ((i < d) && (i < SIDES)); i++) {
      s += (*g)[j][i];
    }
    if ((s < (((d * ((d * DICE) - 1)) / 2) - v)) ||
	(s > (((d * ((d * DICE) - 1)) / 2) + v))) {
      return 0;
    }
  }

  return 1;
}



/* http://permuteweb.tchs.info/
 * Copyright 1991-2008, Phillip Paul Fuchs
 */
int next_perm(int (*g)[DICE][SIDES], int (*p)[DICE + 1],
		      int k, int c) {
  (*p)[k]--;

  int j = k % 2 * (*p)[k];

  int tmp = (*g)[j][c];

  (*g)[j][c] = (*g)[k][c];

  (*g)[k][c] = tmp;

  for (k = 1; (*p)[k] == 0; k++) {
    (*p)[k] = k;
  }

  return k;
}


int getnum(int max, int mask) {

  int rand;
  do {
    rand = (rc4_16_next() & mask);
  }
  while (rand >= max);

  return rand;
}


int add_pop(struct datum *d, int index) {

  /* Try to insert this datum */
  if (insert_or_find_datum(d) == 1) {
    /* we must not have had it previously */

    /* delete the existing one */
    delete_datum(&(population[index]));

    memcpy(&(population[index]), d, sizeof(struct datum));
 
    return 1;
  }

  return 0;
}


double worst_pop_score() {

  int i;
  double s = 0;

  for (i = 0; i < POP_SIZE; i++) {
    if (population[i].score > s) {
      s = population[i].score;
      worst_idx = i;
    }
  }

  return s;
}


void mutate(int (*g)[DICE][SIDES]) {

  int c, r1, r2, temp;

  c = getnum(SIDES, SIDESMASK);
  r1 = getnum(DICE, DICEMASK);

  do {
    r2 = getnum(DICE, DICEMASK);
  } while (r1 == r2);

  temp = (*g)[r1][c];
  (*g)[r1][c] = (*g)[r2][c];
  (*g)[r2][c] = temp;
}


void mutate2(int (*g)[DICE][SIDES]) {

  int c1, c2, r1, r2, temp;

  c1 = getnum(SIDES, SIDESMASK);
  do {
    c2 = getnum(SIDES, SIDESMASK);
  } while (c1 == c2);

  r1 = getnum(DICE, DICEMASK);
  do {
    r2 = getnum(DICE, DICEMASK);
  } while (r1 == r2);

  temp = (*g)[r1][c1];
  (*g)[r1][c1] = (*g)[r2][c1];
  (*g)[r2][c1] = temp;

  temp = (*g)[r1][c2];
  (*g)[r1][c2] = (*g)[r2][c2];
  (*g)[r2][c2] = temp;
}


int compare_datum(const void *a, const void *b,
		  __attribute__((__unused__)) void *param) {

  const struct datum *da = a;
  const struct datum *db = b;

  return memcmp(&(da->grid), &(db->grid), sizeof(grid));
}


int insert_or_find_datum(struct datum *d) {

  struct datum *datum_copy;
  struct datum **datum_probe;

  datum_copy = malloc(sizeof(struct datum));
  memcpy(datum_copy, d, sizeof(struct datum));

  datum_probe = (struct datum **)pavl_probe(pop_tree, datum_copy);

  if (datum_probe == NULL) {
    fprintf(stderr, "There was a failure inserting datum into tree.\n");
    return 0; /* we're screwed so it doesn't matter what we return */
  }

  if (*datum_probe == datum_copy) {
    /* Just inserted, nothing to do */

    /*fprintf(stderr, "Just inserted datum into tree\n");
      print_grid(&(d->grid));*/

    return 1;
  }
  else {
    /* We don't need the datum_copy anymore */
    free(datum_copy);
    datum_copy = NULL;
    return 0;
  }
}


void delete_datum(struct datum *d) {

  struct datum *datum_ret;

  /* Do the deletion */
  datum_ret = (struct datum *)pavl_delete(pop_tree, d);

  free(datum_ret);
}

