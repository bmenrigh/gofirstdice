#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <locale.h>
#include <math.h>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>

#include "rc4_16.h"
#include "pavl.h"

#define MAXTHREADS 4
#define FORKDEPTH  4
#define DOFORK     1
#define SEMSKIP    4096
unsigned char children = 0;
int threadid = 0;
key_t ipc_key;
int semid;
int semret;
int shmid;
struct shared_data *shmdata;
struct sembuf semdown = {0, -1, 0};
struct sembuf semup = {0, 1, 0};


#define DICE      4
#define SIDES     30
#define DICEMASK  7
#define SIDESMASK 31
#define DICEFACT  24
/*#define PERMGOAL  6480000*/      /* N=5; S=60; */
/*#define PERMGOAL  202500*/       /* N=5; S=30; */
#define PERMGOAL  33750            /* N=4; S=30; */
/*#define PERMGOAL  13824*/        /* N=4; S=24; */
/*#define PERMGOAL  4374*/         /* N=4; S=18; */
/*#define PERMGOAL  2304*/
/*#define PERMGOAL  972*/
/*#define PERMGOAL  864*/          /* N=4; S=12; */
/*#define PERMGOAL  288*/
/*#define GOALTALLY 155520000.0*/  /* N=5; S=60; */
/*#define GOALTALLY 4860000.0*/    /* N=5; S=30; */
#define GOALTALLY 202500.0         /* N=4; S=30; */
/*#define GOALTALLY 82944.0*/      /* N=4; S=24; */
/*#define GOALTALLY 26244.0*/      /* N=4; S=18; */
/*#define GOALTALLY 9000.0*/
/*#define GOALTALLY 5184.0*/       /* N=4; S=12; */
/*#define GOALTALLY 4608.0*/
/*#define GOALTALLY 1944.0*/
/*#define GOALTALLY 576.0*/
#define MIRRORSYM 1

#define CLOSEFACTOR 0.0001
/*#define CLOSESCORE (((GOALTALLY * CLOSEFACTOR) *			\
  (GOALTALLY * CLOSEFACTOR)) * (DICE * DICE))*/
#define CLOSESCORE (((PERMGOAL * CLOSEFACTOR) * \
		     (PERMGOAL * CLOSEFACTOR)) * (DICEFACT))


#define POP_SIZE 512 /* Must be power-of-two */
#define POP_MASK (POP_SIZE - 1)

#define SUM_TRIES_LIMIT 100000


int grid[DICE][SIDES];
int g_cache[SIDES][DICE][SIDES];
int g_cache_row[DICE][SIDES][DICE][SIDES];
uint64_t tscore[DICE][DICE];
uint64_t t_cache[SIDES][DICE][DICE];
uint64_t t_cache_row[DICE][SIDES][DICE][DICE];


int selection[DICE];
uint64_t tallies[DICE][DICE];
uint64_t tallie_cache[DICE * SIDES][DICE];
uint64_t min_tallie_left[DICE][SIDES][DICE];
uint64_t max_tallie_left[DICE][SIDES][DICE];
uint64_t permtallies[DICEFACT];
uint64_t all_perms[DICE + 1][DICEFACT];



struct datum {
  uint64_t score;
  int brute;
  int grid[DICE][SIDES];
};

struct pavl_table *perm_tree;

struct perm_node {
  char perm[DICE];
  int amt;
  uint64_t count;
};

struct pavl_table *pop_tree;

/* This will live in an shared memory segment */
struct shared_data {
  int progress;
  uint64_t count_pl_fair;
  uint64_t count_pr_fair;
  uint64_t goal_count;
  uint64_t best_pr_score;
  struct datum population[POP_SIZE];
  uint64_t worst_score;
  uint64_t worst_idx;
  uint64_t best_score;
};
uint64_t t_count_pl_fair;
uint64_t t_best_pr_score;


void distribute(int (*)[DICE][SIDES], int);
void print_grid(int (*)[DICE][SIDES]);
int next_perm(int (*)[DICE][SIDES], int (*)[DICE + 1], int, int);
int is_fair(int (*)[DICE][SIDES], int);
void print_tallies(uint64_t (*)[DICE][DICE]);
void print_perms(uint64_t (*)[DICEFACT]);
void print_all_perms(uint64_t (*)[DICE + 1][DICEFACT]);
void count_perms(int (*)[DICE][SIDES], int, int (*)[DICE],
		 uint64_t (*)[DICE][DICE], uint64_t (*)[DICEFACT]);
void count_places(int (*)[DICE][SIDES], uint64_t (*)[DICE][DICE]);
int check_tallies(uint64_t (*)[DICE][DICE], int);
int check_perms(uint64_t (*)[DICEFACT]);
int score_perms(uint64_t (*)[DICEFACT]);
int score_all_perms(uint64_t (*)[DICE + 1][DICEFACT], uint64_t *);
uint64_t score_tallies(uint64_t (*)[DICE][DICE], uint64_t *);
int check_rows(int (*)[DICE][SIDES], int, int);
int check_pair_fair(int (*)[DICE][SIDES], int, int);
int check_sum(int (*)[DICE][SIDES], int);
void fill_columns2(int (*)[DICE][SIDES], int);
void fill_columns4(int (*)[DICE][SIDES], int);
int getnum(int, int);
int add_pop(struct datum *, int);
uint64_t worst_pop_score();
void climb();
void mutate(int (*)[DICE][SIDES]);
void mutate2(int (*)[DICE][SIDES]);
void mutate_ops(int (*)[DICE][SIDES], int, int, int);
int compare_datum(const void *, const void *, void *);
int insert_or_find_datum(struct datum *);
void delete_datum(struct datum *);
uint64_t choose(int, int);
uint64_t npr(int, int);
void build_tallie_table_math();
void build_tallies_left(int (*)[DICE][SIDES], int);
void sum_find(int (*)[DICE][SIDES], uint64_t (*)[DICE][DICE], int);
void sum_find_row(int (*)[DICE][SIDES], uint64_t (*)[DICE][DICE], int, int,
		  uint64_t *);
int permindex(int (*)[DICE]);
void semwait(int);
void semsignal(int);
void fast_count_perms(int (*)[DICE][SIDES], uint64_t (*)[DICEFACT]);
int compare_pnode(const void *, const void *, void *);
struct perm_node * insert_or_find_pnode(struct pavl_table *,
					struct perm_node *);
void fast_count_perms_print(int (*)[DICE][SIDES]);
void count_all_perms(int (*)[DICE][SIDES], uint64_t (*)[DICE + 1][DICEFACT]);
void acquire_write_lock();
void release_write_lock();
int climb_try_add(struct datum *, int, uint64_t, uint64_t *);

int main(void) {

  int i, j;
  uint64_t linear;
  uint64_t sum_tries;

  struct datum d;

  /* use unitialized stack memory */
  uint16_t rand_key[1000];
  rc4_16_init(&rand_key, 1000);

  /* Before doing anything we need to create our semaphore */

  /* Get the key */
  ipc_key = ftok("dice", 'A');
  if (ipc_key == -1) {
    fprintf(stderr, "ftok failed\n");
    return 1;
  }

  /* Make the semaphores */
  semid = semget(ipc_key, 2, 0644 | IPC_CREAT);
  if (semid == -1) {
    fprintf(stderr, "semget failed\n");
    return 1;
  }

  /* init the semaphores */
  semret = semctl(semid, 0, SETVAL, 1);
  if (semret == -1) {
    fprintf(stderr, "semctl set 0 failed\n");
    return 1;
  }
  semret = semctl(semid, 1, SETVAL, MAXTHREADS);
  if (semret == -1) {
    fprintf(stderr, "semctl set 1 failed\n");
    return 1;
  }

  /* Make the shared memory */
  shmid = shmget(ipc_key, sizeof(struct shared_data), 0644 | IPC_CREAT);
  if (shmid == -1) {
    fprintf(stderr, "shmget failed\n");
    return 1;
  }

  /* Attach the shared memory to this process */
  shmdata = shmat(shmid, (void *)0, 0);
  if (shmdata == (void *)(-1)) {
    fprintf(stderr, "shmat failed\n");
    return 1;
  }

  /* clear the shared memory */
  memset(shmdata, 0, sizeof(struct shared_data));
  shmdata->goal_count = 1;
  shmdata->best_pr_score = (PERMGOAL * DICEFACT) + 1;

  /*for (i = 0; i < 100; i++) {
    printf("%d\n", getnum(SIDES, SIDESMASK));
    }*/

  /* build our tallie table cache */
  build_tallie_table_math();
  build_tallies_left(&grid, 0);

  /* setup the perm tree */
  perm_tree = pavl_create(compare_pnode, NULL, NULL);
  
  /*for (i = 0; i < SIDES; i++) {
    for (j = 0; j < DICE; j++) {
      fprintf(stderr, "Min s=%d, p=%d, v=%ld\n", i, j, min_tallie_left[i][j]);
      fprintf(stderr, "Max s=%d, p=%d, v=%ld\n", i, j, max_tallie_left[i][j]);
    }
    }*/

  /* init the dice */
  if (MIRRORSYM == 0) {
    for (j = 0; j < DICE; j++) {
      for (i = 0; i < SIDES; i++) {
	grid[j][i] = (i * DICE) + j;
      }
    }
  }
  else {
    for (j = 0; j < DICE; j++) {
      for (i = 0; i < (SIDES / 2); i++) {
	grid[j][i] = (i * DICE) + j;
	grid[j][(SIDES - i) - 1] = ((SIDES * DICE) - 1) - ((i * DICE) + j);
      }
    }
  }
    
  /*print_grid(&grid); */

  /* mutate our grid for the hell of it */
  for (i = 0; i < (DICE * SIDES * DICE * SIDES); i++) {
    mutate(&grid);
  }

  /* Setup this datum */
  memcpy(&(d.grid), &grid, sizeof(grid));

  memset(&tallies, 0, sizeof(tallies));
  count_places(&grid, &tallies);
  d.score = score_tallies(&tallies, &linear);
  d.brute = 0;

  /* Setup the population array */
  for (i = 0; i < POP_SIZE; i++) {
    for (j = 0; j < (DICE * SIDES * DICE * SIDES); j++) {
      mutate(&(d.grid));
    }
    memcpy(&(shmdata->population[i]), &d, sizeof(struct datum));
  }
  shmdata->worst_score = worst_pop_score();
  shmdata->best_score = shmdata->worst_score;

  /* Setup the population tree */
  pop_tree = pavl_create(compare_datum, NULL, NULL);
  insert_or_find_datum(&d);

  /*distribute(&grid, 0);*/

  /*climb();*/

  memset(&tscore, 0, sizeof(tscore));
  /*sum_find(&grid, &tscore, 0);*/
  sum_tries = 0;
  sum_find_row(&grid, &tscore, 0, 0, &sum_tries);
  semwait(0);
  fprintf(stderr, "Place fair = %ld; Perm fair = %ld\n",
	  shmdata->count_pl_fair, shmdata->count_pr_fair);
  semsignal(0);


  /*
  fprintf(stderr, "%d npr %d is %ld\n", 0, 0, npr(0, 0));
  fprintf(stderr, "%d choose %d is %ld\n", 49, 7, choose(49, 7));

  fprintf(stderr, "n = %d; p = %d; c = %ld\n", 0, 4, tallie_cache[0][4]);

  for (i = 0; i < 100; i++) {
    mutate(&(population[0].grid));
  }

  memset(&tallies, 0, sizeof(tallies));
  count_perms(&(population[0].grid), 0, &selection, &tallies, DICE);
  print_tallies(&tallies);

  memset(&tallies, 0, sizeof(tallies));
  count_places(&(population[0].grid), &tallies);
  print_tallies(&tallies);*/


  /* Before exiting we need to delete our semaphore */

  /* Remove semaphore */
  if (DOFORK == 1) {
    semret = semctl(semid, 0, IPC_RMID, 0);
    if (semret == -1) {
      fprintf(stderr, "semrm failed\n");
      return 1;
    }
  }

  return 0;
}

void climb() {

  int rp, rm, i, ri;
  uint64_t linear;
  struct datum d;
  struct datum d_tmp, d_tmp2, d_tmp3;
  int last_good;
  int mut_limit;
  uint64_t local_worst;

  /* The brute vars */
  int c1, r1, r2, c2, r21, r22, c3, r31, r32;
  int lim;

  ri = 0;

  int child = 0;
  pid_t status;

  uint16_t *rand_mem;
  uint16_t rand_thrid;

  char *locale;

  linear = 0;

  locale = setlocale(LC_NUMERIC, "en_US.iso88591");
  if (locale == NULL) {
    return;
  }
  
  fprintf(stderr, "The close factor (%f) score is %'ld\n", CLOSEFACTOR,
	  (long)CLOSESCORE);


  last_good = 0;
  mut_limit = ((SIDES * DICE) / 2);
  while (1 == 1) {
    
    /* We'll fork here */
    if ((DOFORK == 1) && (MAXTHREADS > 0) && (child == 0)) {
      /* we're about to make a child */
      children++;

      if (fork() == 0) {
	/* child */
	child = 1;

	fprintf(stderr, "Spawing child #%d\n", threadid);

	/* make sure we're picking different random numbers */
	rand_mem = malloc(1000 * sizeof(uint16_t));
	rc4_16_add((uint16_t (*)[])rand_mem, 1000);
	rand_thrid = threadid;
	rc4_16_add((uint16_t (*)[])&rand_thrid, 1);
      }
      else {
	threadid += 1; /* The next child will get this number */
      
	while (children >= MAXTHREADS) {
	  wait(&status);
	  children--;
	}
	continue;
      }
    } /* end if do fork */

    

    if (threadid != 0) {
      rp = getnum(POP_SIZE, POP_MASK);
      /*fprintf(stderr, "[child %d] about to claim reader sem\n", threadid);*/
      /* we're a reader, down the reader sem */
      semwait(1);
      memcpy(&d, &(shmdata->population[rp]), sizeof(struct datum));
      local_worst = shmdata->worst_score;
      semsignal(1);
    }
    else {
      rp = ri;

      semwait(1);
      memcpy(&d, &(shmdata->population[rp]), sizeof(struct datum));
      local_worst = shmdata->worst_score;
      semsignal(1);

      ri = (ri + 1) % POP_SIZE;
    }
      

    /* do the systematic mutations if they haven't been done yet */
    if (d.brute == 0) {
      if (MIRRORSYM == 1) {
	lim = (SIDES / 2);
      }
      else {
	lim = SIDES;
      }
      for (c1 = 1; c1 < lim; c1++) {
	for (r1 = 0; r1 < (DICE - 1); r1++) {
	  for (r2 = r1 + 1; r2 < DICE; r2++) {
	    memcpy(&d_tmp, &d, sizeof(struct datum));
	    mutate_ops(&(d_tmp.grid), c1, r1, r2);

	    count_all_perms(&(d_tmp.grid), &all_perms);
	    d_tmp.score = score_all_perms(&all_perms, &linear);
      
	    if (d_tmp.score == 0) {
	      acquire_write_lock();
	      printf("[child %d] Got set that is fair for all places\n",
		     threadid);
	      print_grid(&(d_tmp.grid));
	      release_write_lock();
	    }
      
	    if (d_tmp.score < local_worst) {
	      if (climb_try_add(&d_tmp, -1, linear, &local_worst) == 1) {
		last_good = 0;
	      }
	    }
	    
	    /* now do the second-level brute */
	    for (c2 = c1; c2 < lim; c2++) {
	      for (r21 = 0; r21 < (DICE - 1); r21++) {
		for (r22 = r21 + 1; r22 < DICE; r22++) {

		  if ((c2 == c1) && (r21 == r1) && (r22 == r2)) {
		    continue;
		  }

		  memcpy(&d_tmp2, &d_tmp, sizeof(struct datum));
		  mutate_ops(&(d_tmp2.grid), c2, r21, r22);

		  count_all_perms(&(d_tmp2.grid), &all_perms);
		  d_tmp2.score = score_all_perms(&all_perms, &linear);
      
		  if (d_tmp2.score == 0) {
		    acquire_write_lock();
		    printf("[child %d] Got set that is fair for all places\n",
			   threadid);
		    print_grid(&(d_tmp2.grid));
		    release_write_lock();
		  }
      
		  if (d_tmp2.score < local_worst) {
		    if (climb_try_add(&d_tmp2, -2, linear, &local_worst) == 1) {
		      last_good = 0;
		    }
		  }

		  /* now do the third-level brute */
		  for (c3 = c2; c3 < lim; c3++) {
		    for (r31 = 0; r31 < (DICE - 1); r31++) {
		      for (r32 = r31 + 1; r32 < DICE; r32++) {

			if ((c3 == c1) && (r31 == r1) && (r32 == r2)) {
			  continue;
			}
			if ((c3 == c2) && (r31 == r21) && (r32 == r22)) {
			  continue;
			}


			memcpy(&d_tmp3, &d_tmp2, sizeof(struct datum));
			mutate_ops(&(d_tmp3.grid), c3, r31, r32);

			count_all_perms(&(d_tmp3.grid), &all_perms);
			d_tmp3.score = score_all_perms(&all_perms, &linear);
      
			if (d_tmp3.score == 0) {
			  acquire_write_lock();
			  printf("[child %d] Got set that is fair for all places\n",
				 threadid);
			  print_grid(&(d_tmp3.grid));
			  release_write_lock();
			}
      
			if (d_tmp3.score < local_worst) {
			  if (climb_try_add(&d_tmp3, -3, linear, & local_worst) == 1) {
			    last_good = 0;
			  }
			}
		      }
		    }
		  }

		}
	      }
	    }


	  }
	}
      }

      /* mark this datum as bruted */
      acquire_write_lock();
      if (compare_datum(&d, &(shmdata->population[rp]), NULL) == 0) {
	(shmdata->population[rp]).brute = 1;
      }
      release_write_lock();
    }


    /* Start the random mutations */
    if (last_good > (1024 * POP_SIZE)) {
      mut_limit = SIDES * 2;
    }
    else {
      mut_limit = DICE * 2;
    }
    
    mutate(&(d.grid));
    mutate(&(d.grid));
    mutate(&(d.grid));
    for (i = 0; i < mut_limit; i++) {
      /* Do a random mutation */

      if (MIRRORSYM == 0) {
	rm = getnum(2, 1);
	
	if (rm == 0) {
	  mutate(&(d.grid));
	}
	else {
	  mutate2(&(d.grid));
	}
      }
      else {
	mutate(&(d.grid));
      }


      /*count_places(&(d.grid), &tallies);
	d.score = score_tallies(&tallies, &linear);*/
      count_all_perms(&(d.grid), &all_perms);
      d.score = score_all_perms(&all_perms, &linear);
      
      if (d.score == 0) {
	acquire_write_lock();
	printf("[child %d] Got set that is fair for all places\n",
	       threadid);
	print_grid(&(d.grid));
	release_write_lock();
      }
      
      if (d.score < local_worst) {
	if (climb_try_add(&d, i + 1 + 3, linear, &local_worst) == 1) {
	  last_good = 0;
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

      /* Check to see if these are fair for first place */
      memset(&tallies, 0, sizeof(tallies));
      count_places(g, &tallies);
      if (check_tallies(&tallies, DICE) > 0) {
	printf("Got set that is fair for first place\n");
	print_grid(g);
	print_tallies(&tallies);
	/*} */
      }

      memset(&tallies, 0, sizeof(tallies));
      count_places(g, &tallies);
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


void sum_find(int (*g)[DICE][SIDES], uint64_t (*t)[DICE][DICE], int depth) {

  int k, d, pl;
  int p[DICE + 1];
  uint64_t score, linear;

  /*fprintf(stderr, "depth: %d\n", depth); */

  /*if (depth <= 8) {
    fprintf(stderr, "Got to depth %d\n", depth);
    }*/

  if (depth >= 12) {
    fprintf(stderr, "Got to depth %d\n", depth);
  }

  if (((MIRRORSYM == 0) && (depth < SIDES)) ||
      ((MIRRORSYM == 1) && (depth < (SIDES / 2)))) {
    for (d = 0; d < DICE; d++) {
      for (pl = 0; pl < DICE; pl++) {
	if ((*t)[d][pl] + max_tallie_left[0][depth][pl] < GOALTALLY) {
	  /*fprintf(stderr, "Abandoning (under min) at depth %d\n", depth);*/
	  return;
	}
	if ((*t)[d][pl] + min_tallie_left[0][depth][pl] > GOALTALLY) {
	  /*fprintf(stderr, "Abandoning (over max) at depth %d\n", depth);*/
	  return;
	}
      }
    }
  }

  if (depth >= (SIDES)) {
    score = score_tallies(t, &linear);
    fprintf(stderr, "Score: %'ld / %'ld\n", (long)score, (long)linear);
    print_tallies(t);
    fprintf(stderr, "\n");
    return;
  }

  /* save this grid */
  memcpy(&(g_cache[depth]), g, sizeof(grid));

  /* init permutation */
  for (k = 0; k <= DICE; k++) {
    p[k] = k;
  }
  k = 1;

  /* run permutation */

  /* compute new tallie */
  for (d = 0; d < DICE; d++) {
    for (pl = 0; pl < DICE; pl++) {
      t_cache[depth][d][pl] = (*t)[d][pl] +
	tallie_cache[g_cache[depth][d][depth]][pl];
    }
  }
  sum_find(&(g_cache[depth]), &(t_cache[depth]), depth + 1);

  if (depth > 0) {
    while ((k = next_perm(&(g_cache[depth]), &p, k, depth)) != DICE) {

      /* compute new tallie */
      for (d = 0; d < DICE; d++) {
	for (pl = 0; pl < DICE; pl++) {
	  t_cache[depth][d][pl] = (*t)[d][pl] +
	    tallie_cache[g_cache[depth][d][depth]][pl];
	}
      }
      sum_find(&(g_cache[depth]), &(t_cache[depth]), depth + 1);
    }
    
    /* compute new tallie */
    for (d = 0; d < DICE; d++) {
      for (pl = 0; pl < DICE; pl++) {
	t_cache[depth][d][pl] = (*t)[d][pl] +
	  tallie_cache[g_cache[depth][d][depth]][pl];
      }
    }
    sum_find(&(g_cache[depth]), &(t_cache[depth]), depth + 1);
  }


}


void sum_find_row(int (*g)[DICE][SIDES], uint64_t (*t)[DICE][DICE],
		  int row, int depth, uint64_t *c) {

  int d, pl, i;
  pid_t status;
  uint64_t pr_score;
  /*double score, linear;*/

  /* keep track of tries and bail out */
  /**c += 1;
  if (*c > SUM_TRIES_LIMIT) {
    return;
    }*/

  /* We gotta build our tallie left cache for this row */
  if ((depth == 0) && (row != DICE)) {
    build_tallies_left(g, row);
  }

  
  if (((MIRRORSYM == 0) && (depth < SIDES)) ||
      ((MIRRORSYM == 1) && (depth < (SIDES / 2)))) {
    for (pl = 0; pl < DICE; pl++) {
      if ((*t)[row][pl] + max_tallie_left[row][depth][pl] < GOALTALLY) {
	/*fprintf(stderr, "Abandoning (under min) at depth %d, row %d\n",
	  depth, row);*/
	return;
      }
      if ((*t)[row][pl] + min_tallie_left[row][depth][pl] > GOALTALLY) {
	/*fprintf(stderr, "Abandoning (over max) at depth %d, row %d\n",
	  depth, row);*/
	return;
      }
    }
  }
  
  /* fork the shit out of this problem */
  if ((DOFORK == 1) && (row == 0) && (depth == FORKDEPTH) &&
      (MAXTHREADS > 0)) {
    /* we're about to make a child */
    children++;

    threadid += 1; /* The next child will get this number */

    if (fork() == 0) {
      /* child */
    }
    else {
      threadid += 1; /* The next child will get this number */
      while (children >= MAXTHREADS) {
	wait(&status);
	children--;
      }
      return;
    }
  } /* end if depth == FORKDEPTH */


  /*
  if ((row + 1) * (depth + 1) > progress) {
    fprintf(stderr, "\nRecord progress (row=%d, depth=%d, c=%lu)\n",
	    row, depth, *c);
    progress = (row + 1) * (depth + 1);
    print_grid(g);
    print_tallies(t);
    }*/



  if (depth >= (SIDES)) {
    /*print_grid(g);
      return;*/
    for (pl = 0; pl < DICE; pl++) {
      if ((*t)[row][pl] != GOALTALLY) {
	return;
      }
    }

    /* Check pair-wise fairness up to this point */
    for (i = 0; i < row; i++) {
      if (check_pair_fair(g, i, row) != 1) {
	return;
      }
    }

    
    if (row == (DICE - 1)) {
      
      t_count_pl_fair += 1;
      
      if (t_count_pl_fair >= SEMSKIP) {
	semwait(0);
	shmdata->count_pl_fair += t_count_pl_fair;
	
	if (shmdata->count_pl_fair >= shmdata->goal_count) {
	  fprintf(stderr, "[child %d] Place-fair count so far: %lu\n",
		  threadid, shmdata->count_pl_fair);
	  
	  shmdata->goal_count *= 2;
	}
	/*print_tallies(t);*/
	t_count_pl_fair = 0;
	t_best_pr_score = shmdata->best_pr_score;
	
	semsignal(0);
      }
      
      /*fprintf(stderr, "%lu\n", count);*/

      memset(&selection, 0, sizeof(selection));
      memset(&permtallies, 0, sizeof(permtallies));
      memset(&tallies, 0, sizeof(tallies));

      fast_count_perms(g, &permtallies);
	
      /*count_perms(g, 0, &selection,
	&tallies, &permtallies);*/
      pr_score = score_perms(&permtallies);

      if (pr_score < t_best_pr_score) {
	semwait(0);
	if (pr_score < shmdata->best_pr_score) {
	  fprintf(stderr, "[child %d] Best perm-score so far: %lu\n",
		  threadid, pr_score);
	  print_perms(&permtallies);
	  shmdata->best_pr_score = pr_score;
	  
	}

	t_best_pr_score = shmdata->best_pr_score;

	fflush(stdout);
	semsignal(0);
      }

      if (pr_score == 0) {
	
	/* aquire lock */

	semwait(0);
	shmdata->count_pr_fair += 1;

	/*score = score_tallies(t, &linear);
	  fprintf(stderr, "Score: %'ld / %'ld\n", (long)score, (long)linear);*/
	print_grid(g);
	/*print_tallies(t);*/
	/*fprintf(stderr, "\n");*/
	/*fast_count_perms_print(g);*/

	fflush(stdout);
	/* release lock */
	semsignal(0);

      }
      else {
	/*fprintf(stderr, "Perms were bad...\n");
	for (i = 0; i < DICEFACT; i++) {
	  fprintf(stderr, "%ld ", permtallies[i]);
	}
	fprintf(stderr, "\n");*/
      }
      return;
    }
    else {

      /*fprintf(stderr, "Moving on to row %d\n", row + 1);*/
      sum_find_row(g, t, row + 1, 0, c);
      return;
    }

    /* Should not be reachable */
    return; /* we're over depth max, can't go on */
  }

  /* Just assign this row a value if we're doing mirror symmetry */
  if (MIRRORSYM == 1) {
    if (depth >= (SIDES / 2)) {

      /* save this grid */
      memcpy(&(g_cache_row[row][depth]), g, sizeof(grid));

      /* assing this the value based on the mirror row */
      g_cache_row[row][depth][row][depth] =
	((DICE * SIDES) - 1) - (*g)[row][(SIDES / 2) -
					 ((depth - (SIDES / 2)) + 1)];
      
      memcpy(&(t_cache_row[row][depth]), t, sizeof(tallies));
      /* compute new tallie */
      /*for (pl = 0; pl < DICE; pl++) {
	t_cache_row[row][depth][row][pl] = (*t)[row][pl] +
	  tallie_cache[g_cache_row[row][depth][row][depth]][pl];
	  }*/

      sum_find_row(&(g_cache_row[row][depth]), &(t_cache_row[row][depth]),
		   row, depth + 1, c);
      return;
    }
  }


  /* Swap this cell with any of the cells from rows below it */
  for (d = row; d < DICE; d++) {
    /* save this grid */
    memcpy(&(g_cache_row[row][depth]), g, sizeof(grid));

    g_cache_row[row][depth][row][depth] = (*g)[d][depth];
    g_cache_row[row][depth][d][depth] = (*g)[row][depth];

    memcpy(&(t_cache_row[row][depth]), t, sizeof(tallies));
    /* compute new tallie */
    for (pl = 0; pl < DICE; pl++) {
      t_cache_row[row][depth][row][pl] = (*t)[row][pl] +
	tallie_cache[g_cache_row[row][depth][row][depth]][pl];
    }
    if (MIRRORSYM == 1) {
      for (pl = 0; pl < DICE; pl++) {
	t_cache_row[row][depth][row][pl] += 
	  tallie_cache[((DICE * SIDES) - 1) -
		       g_cache_row[row][depth][row][depth]][pl];
      }
    }
    
    sum_find_row(&(g_cache_row[row][depth]), &(t_cache_row[row][depth]),
		 row, depth + 1, c);

    /* Don't bother permuting the first column */
    if (depth == 0) {
      break;
    }
  }

  /* Don't let our child go below where it was spawned */
  if ((DOFORK == 1) && (row == 0) && (depth == FORKDEPTH)) {
    exit(0);
  }

  if ((DOFORK == 1) && (row == 0) && (depth == 0)) {
    while (children > 0) {
      wait(&status);
      children--;
    }
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


int permindex(int (*p)[DICE]) {

  int i, j, pidx;
  int perm[DICE];

  memcpy(&perm, p, sizeof(perm));
  /* find the index for this permutation */
  for (i = 0; i < (DICE - 1); i++) {
    for (j = i + 1; j < DICE; j++) {
      if (perm[j] > perm[i]) {
	perm[j] -= 1;
      }
    }
  }
  pidx = 0;
  j = (DICE - 1);
  for (i = 0; i < (DICE - 1); i++) {
    pidx = j * (pidx + perm[i]);
    j--;
  }

  return pidx;
}


void count_perms(int (*g)[DICE][SIDES], int depth,
		 int (*s)[DICE], uint64_t (*t)[DICE][DICE],
		 uint64_t (*p)[DICEFACT]) {

  int i, i2, count, pidx;
  int perm[DICE];

  if (depth == DICE) {
    for (i = 0; i < DICE; i++) {
      count = 0;

      for (i2 = 0; i2 < DICE; i2++) {
	if ((*s)[i] < (*s)[i2]) {
	  count++;
	}
      }
      (*t)[i][count] += 1;

      perm[count] = i;
    }

    pidx = permindex(&perm);
      
    /* Add one to this perm bucket */
    (*p)[pidx] += 1;

    return;
  }

  for (i = 0; i < SIDES; i++) {
    (*s)[depth] = (*g)[depth][i];
    count_perms(g, depth + 1, s, t, p);
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


int check_sum(int (*g)[DICE][SIDES], int d) {

  int i, s;

  s = 0;
  for (i = 0; i < SIDES; i++) {
    s += (*g)[d][i];
  }

  if (s == (((SIDES * DICE) * ((SIDES * DICE) - 1)) / (2 * DICE))) {
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


int check_tallies(uint64_t (*t)[DICE][DICE], int d) {

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


int check_perms(uint64_t (*p)[DICEFACT]) {

  int i;

  for (i = 0; i < DICEFACT; i++) {

    if ((*p)[i] != PERMGOAL) {
      return 0;
    }
  }
 
  return 1;
}


int score_perms(uint64_t (*p)[DICEFACT]) {

  int i, t;
  uint64_t s;

  s = 0;
  for (i = 0; i < DICEFACT; i++) {

    t = PERMGOAL - (*p)[i];
    if (t < 0) {
      t *= -1;
    }

    s += t;
  }
 
  return s;
}


int score_all_perms(uint64_t (*p)[DICE + 1][DICEFACT], uint64_t *linear) {

  int i, n, c;
  int64_t t;
  uint64_t s, pgoal, factor;

  factor = 1 << (DICE + 1);

  s = 0;
  *linear = 0;
  for (n = 0; n <= DICE; n++) {
    c = npr(DICE, n);
    pgoal = ((uint64_t)pow(SIDES, n)) / npr(n, n);

    factor >>= 1;

    for (i = 0; i < c; i++) {

      t = pgoal - (*p)[n][i];
      if (t < 0) {
	t *= -1;
      }

      s += (t * factor * factor);
      if (n == DICE) {
	*linear += t;
      }
    }
  }
 
  return s;
}


uint64_t score_tallies(uint64_t (*t)[DICE][DICE], uint64_t *linear) {

  int i, j;
  uint64_t s;
  int64_t d;

  s = 0;
  *linear = 0;
  for (i = 0; i < DICE; i++) {
    for (j = 0; j < DICE; j++) {
      
      d = (((*t)[j][i] - GOALTALLY));

      if (d < 0) {
	d *= -1;
      }

      s += (d * d);
      *linear += d;
	   
    }
  }
 
  return s;
}


void print_tallies(uint64_t (*t)[DICE][DICE]) {

  int i, j;

  printf("----------------------\n");

  for (j = 0; j < DICE; j++) {
    for (i = 0; i < DICE; i++) {
      printf("%-5ld ", (*t)[j][i]);
    }
    printf("\n");
  }

}


void print_perms(uint64_t (*p)[DICEFACT]) {

  int i;

  fprintf(stderr, "+++++++++++++++++++++++++++++++\n");

  for (i = 0; i < DICEFACT; i++) {
    fprintf(stderr, "%-7ld ", (*p)[i]);
    
    if ((i + 1) % (DICE * 2) == 0) {
      fprintf(stderr, "\n");
    }
  }

}


void print_all_perms(uint64_t (*p)[DICE + 1][DICEFACT]) {

  int i, n, c;

  fprintf(stderr, "+++++++++++++++++++++++++++++++\n");

  for (n = 0; n <= DICE; n++) {
    c = npr(DICE, n);

    fprintf(stderr, "-- %d --\n", n);

    for (i = 0; i < c; i++) {
      fprintf(stderr, "%-7ld ", (*p)[n][i]);
    
      if ((i + 1) % (DICE * 2) == 0) {
	fprintf(stderr, "\n");
      }
    }
    if ((c) % (DICE * 2) != 0) {
      fprintf(stderr, "\n");
    }

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

  int i, exists;

  if (DOFORK == 0) {
    /* Try to insert this datum */
    if (insert_or_find_datum(d) == 1) {
      /* we must not have had it previously */

      /* delete the existing one */
      delete_datum(&(shmdata->population[index]));

      memcpy(&(shmdata->population[index]), d, sizeof(struct datum));
      (shmdata->population[index]).brute = 0;
 
      return 1;
    }
  } else {
    /* Can't use the tree because malloc() isn't going to share that mem
     * across threads so the tree will be out-of-date for other threads
     */
    exists = 0;
    for (i = 0; i < POP_SIZE; i++) {
      if (compare_datum(d, &(shmdata->population[i]), NULL) == 0) {
	exists = 1;
	break;
      }
    }
    if (exists == 0) {
      memcpy(&(shmdata->population[index]), d, sizeof(struct datum));
      (shmdata->population[index]).brute = 0;
      return 1;
    }
  }

  return 0;
}


uint64_t worst_pop_score() {

  int i;
  uint64_t s = 0;

  for (i = 0; i < POP_SIZE; i++) {
    if (shmdata->population[i].score > s) {
      s = shmdata->population[i].score;
      shmdata->worst_idx = i;
    }
  }

  return s;
}


void mutate(int (*g)[DICE][SIDES]) {

  int c, r1, r2, temp, c2;

  if (MIRRORSYM == 0) {
    c = getnum(SIDES - 1, SIDESMASK) + 1;
    r1 = getnum(DICE, DICEMASK);

    do {
      r2 = getnum(DICE, DICEMASK);
    } while (r1 == r2);

    temp = (*g)[r1][c];
    (*g)[r1][c] = (*g)[r2][c];
    (*g)[r2][c] = temp;
  }
  else {
    c = getnum((SIDES / 2) - 1, ((SIDESMASK + 1) / 2) - 1) + 1;
    r1 = getnum(DICE, DICEMASK);

    do {
      r2 = getnum(DICE, DICEMASK);
    } while (r1 == r2);

    temp = (*g)[r1][c];
    (*g)[r1][c] = (*g)[r2][c];
    (*g)[r2][c] = temp;

    c2 = (SIDES - c) - 1;
    if (c != c2) {
      temp = (*g)[r1][c2];
      (*g)[r1][c2] = (*g)[r2][c2];
      (*g)[r2][c2] = temp;
    }
  }
}


void mutate2(int (*g)[DICE][SIDES]) {

  int c1, c2, r1, r2, temp;

  if (MIRRORSYM == 1) {
    mutate(g);
    return;
  }

  c1 = getnum(SIDES - 1, SIDESMASK) + 1;
  do {
    c2 = getnum(SIDES - 1, SIDESMASK) + 1;
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


void mutate_ops(int (*g)[DICE][SIDES], int c, int r1, int r2) {

  int temp, c2;

  if (MIRRORSYM == 0) {

    temp = (*g)[r1][c];
    (*g)[r1][c] = (*g)[r2][c];
    (*g)[r2][c] = temp;
  }
  else {

    temp = (*g)[r1][c];
    (*g)[r1][c] = (*g)[r2][c];
    (*g)[r2][c] = temp;

    c2 = (SIDES - c) - 1;
    if (c != c2) {
      temp = (*g)[r1][c2];
      (*g)[r1][c2] = (*g)[r2][c2];
      (*g)[r2][c2] = temp;
    }
  }
}


int compare_datum(const void *a, const void *b, void *param) {

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

uint64_t choose(int n, int r) {

  uint64_t c, nr, l1, l2;
  int i;

  if (r > n) {
    return 0;
  }
  if (n == r) {
    return 1;
  }

  nr = n - r;

  /* figure out which limit is larger and set l1 to that */
  if (r > nr) {
    l1 = r;
    l2 = nr;
  }
  else {
    l1 = nr;
    l2 = r;
  }

  c = 1;
  for (i = n; i > l1; i--) {
    c *= i;
  }
  for (i = l2; i > 1; i--) {
    c /= i;
  }

  return c;
}


uint64_t npr(int n, int r) {

  uint64_t c, nr;
  int i;

  if (r > n) {
    return 0;
  }
  if (n == 0) {
    return 1;
  }

  nr = n - r;

  c = 1;
  for (i = n; i > nr; i--) {
    c *= i;
  }

  return c;
}


void build_tallie_table_math() {

  int d, s, p, v, ab;

  for (s = 0; s < SIDES; s++) {
    for (d = 0; d < DICE; d++) {
     
      v = (s * DICE) + d;
      
      /* Now figure out for each place, starting with first */
      for (p = 0; p < DICE; p++) {
	/* to be in 1st place (0th) there needs to be DICE - 1 numbers
	 * under and 0 nums above */

	/* to be in p'th place there needs to be (DICE - 1) - p under
	 * and p over you */

	/* You can choose dice - 1 choose n ways to pick from the
	 * before or after dice */

	/* ab is the number of faces to choose from the rows above this d */
	for (ab = 0; ((ab <= p) && (ab <= d)); ab++) {
	  tallie_cache[v][p] +=
	    ((choose(d, ab) *
	      (uint64_t)pow((SIDES - s) - 1, ab)) *
	     (choose((DICE - 1) - d, p - ab) *
	      (uint64_t)pow(SIDES - s, p - ab)) *
	     (uint64_t)pow(s + 1, d - ab) *
	     (uint64_t)pow(s, ((DICE - d) - 1) - (p - ab)));
	}

	/*fprintf(stderr, "n=%d, p=%d, c=%ld\n", (s * DICE) + d, p, tallie_cache[v][p]);*/
      }
    }
  }

}


void count_places(int (*g)[DICE][SIDES], uint64_t (*t)[DICE][DICE]) {

  int d, s, p;

  for (d = 0; d < DICE; d++) {
    for (s = 0; s < SIDES; s++) {

      for (p = 0; p < DICE; p++) {
	(*t)[d][p] += tallie_cache[(*g)[d][s]][p];
      }
    }
  }
}


void build_tallies_left(int (*g)[DICE][SIDES], int rows) {

  int d, s, p, v, t, l, i, skip;

  uint64_t min, max;

  if (MIRRORSYM == 1) {
    l = ((SIDES / 2) - 1);
  }
  else {
    l = (SIDES - 1);
  }

  for (p = 0; p < DICE; p++) { /* for each place */
    for (s = l; s >= 0; s--) { /* start from last to first */
      min = (uint64_t)pow(SIDES, DICE - 1);
      max = 0;
      for (d = 0; d < DICE; d++) { /* for each dice you could pick */
	v = (s * DICE) + d;

	/* I've we've already placed this number, skip it */
	skip = 0;
	for (i = 0; i < rows; i++) {
	  if ((*g)[i][s] == v) {
	    skip = 1;
	    break;
	  }
	}
	if (skip == 1) {
	  continue;
	}

	t = tallie_cache[v][p];
	if (MIRRORSYM == 1) {
	  t += tallie_cache[((DICE * SIDES) - 1) - v][p];
	}

	if (t < min) {
	  min = t;
	}
	if (t > max) {
	  max = t;
	}
      } /* end for d */

      if (s == l) {
	min_tallie_left[rows][s][p] = min;
	max_tallie_left[rows][s][p] = max;
      }
      else {
	min_tallie_left[rows][s][p] = min + min_tallie_left[rows][s + 1][p];
	max_tallie_left[rows][s][p] = max + max_tallie_left[rows][s + 1][p];
      }
    } /* end for s */
  }
}


void semwait(int n) {

  semdown.sem_num = n;

  if (DOFORK == 1) {
    semret = semop(semid, &semdown, 1);
    if (semret == -1) {
      fprintf(stderr, "semdown failed\n");
      exit(1);
    }
  }
}


void semsignal(int n) {

  semup.sem_num = n;

  if (DOFORK == 1) {
    semret = semop(semid, &semup, 1);
    if (semret == -1) {
      fprintf(stderr, "semup failed\n");
      exit(1);
    }
  }
}


void fast_count_perms(int (*g)[DICE][SIDES], uint64_t (*p)[DICEFACT]) {

  struct perm_node t;
  struct perm_node *tp;

  struct pavl_traverser traverser;
  struct perm_node *pnode_last;
  struct perm_node *pnode_cur;

  int n, d, c, i, has_d;

  /* the empty perm starts with a count of 1 */
  t.amt = -1;
  tp = insert_or_find_pnode(perm_tree, &t);
  tp->count = 1;

  /* We do the numbers in order */
  for (n = 0; n < (DICE * SIDES); n++) {
    /* compute the column we're in */
    c = n / DICE;

    /* find which dice has this n */
    for (d = 0; d < DICE; d++) {
      if ((*g)[d][c] == n) {

	/* okay for each permutation in our tree that doesn't yet have
	 * this dice, d, in it, we need to add it onto the end
	 */
	pavl_t_init(&traverser, perm_tree);

	pnode_last = (struct perm_node *)pavl_t_next(&traverser);
	pnode_cur = (struct perm_node *)pavl_t_next(&traverser);
	while (pnode_last != NULL) {

	  /*fprintf(stderr, "in loop\n");*/

	  has_d = 0;
	  if (pnode_last->amt < DICE) {
	    for (i = 0; i <= pnode_last->amt; i++) {
	      /*fprintf(stderr, "in loop 2\n");*/
	      if (pnode_last->perm[i] == d) {
		has_d = 1;
	      }
	    }
	  }


	  if (has_d == 0) {
	    /*fprintf(stderr, "doesn't have d\n");*/
	    /* We found a perm we need to put d onto the end of */
	    memcpy(&t, pnode_last, sizeof(struct perm_node));
	    t.count = 0;
	    t.perm[t.amt + 1] = d;
	    t.amt += 1;
	   
	    /* Now stick it in or find this perm in the tree */
	    tp = insert_or_find_pnode(perm_tree, &t);

	    /* The only thing to do now is to add this count */
	    tp->count += pnode_last->count;
	  }	    

	  /* Move on */
	  /*fprintf(stderr, "about to move on\n");*/
	  pnode_last = pnode_cur;
	  pnode_cur = (struct perm_node *)pavl_t_next(&traverser);
	  /*fprintf(stderr, "moving on\n");*/
	}

	break;
      }
    }
  }
  /*fprintf(stderr, "made it out\n");*/

  /* Now loop through the full perms */
  pavl_t_init(&traverser, perm_tree);

  pnode_last = (struct perm_node *)pavl_t_next(&traverser);
  pnode_cur = (struct perm_node *)pavl_t_next(&traverser);

  i = 0;
  while (pnode_last != NULL) {

    if (pnode_last->amt == (DICE - 1)) {
      /*fprintf(stderr, "permcount: %lu\n", pnode_last->count);*/
      (*p)[i] = pnode_last->count;
      i++;
    }

    /* reset this for next time */
    pnode_last->count = 0;

    /* Move on */
    pnode_last = pnode_cur;
    pnode_cur = (struct perm_node *)pavl_t_next(&traverser);
  }
}


int compare_pnode(const void *a, const void *b, void *param) {

  const struct perm_node *pa = a;
  const struct perm_node *pb = b;

  if (pa->amt < pb->amt) {
    return -1;
  }
  else if (pa->amt > pb->amt) {
    return 1;
  }
  else {
    if (pa->amt == -1) {
      return 0;
    }
    return memcmp(&(pa->perm), &(pb->perm), (pa->amt + 1));
  }
}


struct perm_node * insert_or_find_pnode(struct pavl_table *t,
					struct perm_node *p) {

  struct perm_node *pnode_copy;
  struct perm_node **pnode_probe;

  pnode_copy = malloc(sizeof(struct perm_node));
  memcpy(pnode_copy, p, sizeof(struct perm_node));

  /*fprintf(stderr, "about to probe\n");*/
  pnode_probe = (struct perm_node **)pavl_probe(t, pnode_copy);
  /*fprintf(stderr, "probe done\n");*/

  if (pnode_probe == NULL) {
    fprintf(stderr, "There was a failure inserting perm_node into tree.\n");
    return NULL; /* we're screwed so it doesn't matter what we return */
  }

  if (*pnode_probe == pnode_copy) {
    /* Just inserted, nothing to do */

    /*fprintf(stderr, "Just inserted datum into tree\n");
      print_grid(&(d->grid));*/

    return *pnode_probe;
  }
  else {
    /* We don't need the datum_copy anymore */
    free(pnode_copy);
    pnode_copy = NULL;
    return *pnode_probe;
  }
}


void fast_count_perms_print(int (*g)[DICE][SIDES]) {

  struct perm_node t;
  struct perm_node *tp;

  struct pavl_traverser traverser;
  struct perm_node *pnode_last;
  struct perm_node *pnode_cur;

  int n, d, c, i, has_d, j;

  /* the empty perm starts with a count of 1 */
  t.amt = -1;
  tp = insert_or_find_pnode(perm_tree, &t);
  tp->count = 1;

  /* We do the numbers in order */
  for (n = 0; n < (DICE * SIDES); n++) {
    /* compute the column we're in */
    c = n / DICE;

    /* find which dice has this n */
    for (d = 0; d < DICE; d++) {
      if ((*g)[d][c] == n) {

	/* okay for each permutation in our tree that doesn't yet have
	 * this dice, d, in it, we need to add it onto the end
	 */
	pavl_t_init(&traverser, perm_tree);

	pnode_last = (struct perm_node *)pavl_t_next(&traverser);
	pnode_cur = (struct perm_node *)pavl_t_next(&traverser);
	while (pnode_last != NULL) {

	  /*fprintf(stderr, "in loop\n");*/

	  has_d = 0;
	  if (pnode_last->amt < DICE) {
	    for (i = 0; i <= pnode_last->amt; i++) {
	      /*fprintf(stderr, "in loop 2\n");*/
	      if (pnode_last->perm[i] == d) {
		has_d = 1;
	      }
	    }
	  }


	  if (has_d == 0) {
	    /*fprintf(stderr, "doesn't have d\n");*/
	    /* We found a perm we need to put d onto the end of */
	    memcpy(&t, pnode_last, sizeof(struct perm_node));
	    t.count = 0;
	    t.perm[t.amt + 1] = d;
	    t.amt += 1;
	   
	    /* Now stick it in or find this perm in the tree */
	    tp = insert_or_find_pnode(perm_tree, &t);

	    /* The only thing to do now is to add this count */
	    tp->count += pnode_last->count;
	  }	    

	  /* Move on */
	  /*fprintf(stderr, "about to move on\n");*/
	  pnode_last = pnode_cur;
	  pnode_cur = (struct perm_node *)pavl_t_next(&traverser);
	  /*fprintf(stderr, "moving on\n");*/
	}

	break;
      }
    }
  }
  /*fprintf(stderr, "made it out\n");*/

  for (j = 0; j < DICE; j++) {
    pavl_t_init(&traverser, perm_tree);

    pnode_last = (struct perm_node *)pavl_t_next(&traverser);
    pnode_cur = (struct perm_node *)pavl_t_next(&traverser);

    while (pnode_last != NULL) {

      if (pnode_last->amt == j) {
	fprintf(stderr, "%d permcount: %lu\n", j, pnode_last->count);

	/* reset this for next time */
	pnode_last->count = 0;
      }
      
      
      /* Move on */
      pnode_last = pnode_cur;
      pnode_cur = (struct perm_node *)pavl_t_next(&traverser);
    }
  }
}


void count_all_perms(int (*g)[DICE][SIDES],
		     uint64_t (*p)[DICE + 1][DICEFACT]) {

  struct perm_node t;
  struct perm_node *tp;

  struct pavl_traverser traverser;
  struct perm_node *pnode_last;
  struct perm_node *pnode_cur;

  int n, d, c, i, has_d;
  int idx[DICE + 1];

  /* the empty perm starts with a count of 1 */
  t.amt = -1;
  tp = insert_or_find_pnode(perm_tree, &t);
  tp->count = 1;

  /* We do the numbers in order */
  for (n = 0; n < (DICE * SIDES); n++) {
    /* compute the column we're in */
    c = n / DICE;

    /* find which dice has this n */
    for (d = 0; d < DICE; d++) {
      if ((*g)[d][c] == n) {

	/* okay for each permutation in our tree that doesn't yet have
	 * this dice, d, in it, we need to add it onto the end
	 */
	pavl_t_init(&traverser, perm_tree);

	pnode_last = (struct perm_node *)pavl_t_next(&traverser);
	pnode_cur = (struct perm_node *)pavl_t_next(&traverser);
	while (pnode_last != NULL) {

	  /*fprintf(stderr, "in loop\n");*/

	  has_d = 0;
	  if (pnode_last->amt < DICE) {
	    for (i = 0; i <= pnode_last->amt; i++) {
	      /*fprintf(stderr, "in loop 2\n");*/
	      if (pnode_last->perm[i] == d) {
		has_d = 1;
	      }
	    }
	  }


	  if (has_d == 0) {
	    /*fprintf(stderr, "doesn't have d\n");*/
	    /* We found a perm we need to put d onto the end of */
	    memcpy(&t, pnode_last, sizeof(struct perm_node));
	    t.count = 0;
	    t.perm[t.amt + 1] = d;
	    t.amt += 1;
	   
	    /* Now stick it in or find this perm in the tree */
	    tp = insert_or_find_pnode(perm_tree, &t);

	    /* The only thing to do now is to add this count */
	    tp->count += pnode_last->count;
	  }	    

	  /* Move on */
	  /*fprintf(stderr, "about to move on\n");*/
	  pnode_last = pnode_cur;
	  pnode_cur = (struct perm_node *)pavl_t_next(&traverser);
	  /*fprintf(stderr, "moving on\n");*/
	}

	break;
      }
    }
  }
  /*fprintf(stderr, "made it out\n");*/

  /* Now loop through the full perms */
  pavl_t_init(&traverser, perm_tree);

  pnode_last = (struct perm_node *)pavl_t_next(&traverser);
  pnode_cur = (struct perm_node *)pavl_t_next(&traverser);

  memset(&idx, 0, sizeof(idx));
  while (pnode_last != NULL) {

    /* store this perm in the right perm length spot */
    (*p)[pnode_last->amt + 1][idx[pnode_last->amt + 1]] = pnode_last->count;
    idx[pnode_last->amt + 1] += 1;

    /* reset this for next time */
    pnode_last->count = 0;

    /* Move on */
    pnode_last = pnode_cur;
    pnode_cur = (struct perm_node *)pavl_t_next(&traverser);
  }
}


void acquire_write_lock() {

  int s;

  /*We're a writer, we need to claim all spots */
  semwait(0); /* the writer lock */
  for (s = 0; s < MAXTHREADS; s++) {
    semwait(1);  /* gets one at a time */
  }
}

void release_write_lock() {

  int s;

  /* we're done writing / outputting */
  for (s = 0; s < MAXTHREADS; s++) {
    semsignal(1);  /* gets one at a time */
  }
  semsignal(0);
}


int climb_try_add(struct datum *d, int m, uint64_t linear, uint64_t *worst) {

  uint64_t new_worst;
  int added;

  /* get the write lock */
  acquire_write_lock();

  if (d->score >= shmdata->worst_score) {
    added = 0;
    goto unlock_and_continue;
  }

  /*fprintf(stderr, "Adding with %d mutations\n", i);*/
  if (add_pop(d, shmdata->worst_idx) == 0) {
    /*print_grid(&(d.grid)); */
    added = 0;
    goto unlock_and_continue;
  }
  added = 1;
	
  new_worst = worst_pop_score();

  if (d->score < shmdata->best_score) {
    shmdata->best_score = d->score;
    fprintf(stderr, "\n[child %d] New best score: %'ld / %'ld"
	    " (worst: %'ld) (mutations: %d)\n",
	    threadid,
	    (long)shmdata->best_score, (long)linear,
	    (long)shmdata->worst_score, m);
    /*print_grid(&(d->grid));*/
    /*print_tallies(&tallies);*/
    print_all_perms(&all_perms);
    
  }
  if (new_worst < shmdata->worst_score) {
    shmdata->worst_score = new_worst;
    /*fprintf(stderr, "New worst score: %0.3f\n", worst_score);*/
  }

 unlock_and_continue:
  *worst = shmdata->worst_score;
  release_write_lock();

  return added;
}
