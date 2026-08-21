#define _POSIX_C_SOURCE 200809L

/*
 * Exhaustive search for column-grouped go-first dice.
 *
 * DICE and SIDES deliberately are compile-time constants.  Override the
 * defaults with, for example:
 *
 *   cc -std=c11 -O3 -march=native -flto \
 *      -DDICE=4 -DSIDES=18 -DMIRROR=1 -DHIGH_FIRST=1 \
 *      -o column_search column_search.c
 *
 * A column contains DICE consecutive labels and assigns one label to each
 * die.  Under that restriction, a face's contribution to every finishing
 * place is independent of all other columns.  Those contributions are
 * computed once and accumulated during the search.
 */

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#ifndef DICE
#define DICE 4
#endif

#ifndef SIDES
#define SIDES 12
#endif

#ifndef MIRROR
#define MIRROR 0
#endif

#ifndef HIGH_FIRST
#define HIGH_FIRST 1
#endif

#ifndef PERM_ONLY
#define PERM_ONLY 0
#endif

#ifndef ADDITIVE_PERM_LINEAR
#define ADDITIVE_PERM_LINEAR 1
#endif

#ifndef ROW2_MITM
#define ROW2_MITM 0
#endif

#ifndef ROW1_MITM
#define ROW1_MITM 0
#endif

#ifndef CONDITIONED_COMPLETION_BOUNDS
#define CONDITIONED_COMPLETION_BOUNDS 0
#endif

#ifndef EARLY_CONDITIONED_COMPLETION_BOUNDS
#define EARLY_CONDITIONED_COMPLETION_BOUNDS 1
#endif

#ifndef INCREMENTAL_C_COMPLETION
#define INCREMENTAL_C_COMPLETION 0
#endif

#ifndef INCREMENTAL_C_COMPLETION_DIRECTIONS
#define INCREMENTAL_C_COMPLETION_DIRECTIONS 1
#endif

#ifndef INCREMENTAL_C_COMPLETION_COUPLED
#define INCREMENTAL_C_COMPLETION_COUPLED 0
#endif

#ifndef MIRROR_COLUMNS
#if MIRROR
#define MIRROR_COLUMNS (SIDES / 2)
#else
#define MIRROR_COLUMNS 0
#endif
#endif

/* The outer K pairs are forced; the middle SIDES-2K columns stay free. */
#define FACE_COUNT (DICE * SIDES)
#define SEARCH_COLUMNS (SIDES - MIRROR_COLUMNS)
#define UNMIRRORED_COLUMNS (SIDES - 2 * MIRROR_COLUMNS)
#define FULL_MIRROR (MIRROR_COLUMNS * 2 == SIDES)
#define PAIR_GOAL (UNMIRRORED_COLUMNS / 2U)

#if MIRROR != 0 && MIRROR != 1
#error "MIRROR must be either 0 or 1"
#endif

#if MIRROR_COLUMNS < 0 || MIRROR_COLUMNS * 2 > SIDES
#error "MIRROR_COLUMNS must be between zero and SIDES/2"
#endif

#if MIRROR && MIRROR_COLUMNS * 2 != SIDES
#error "MIRROR=1 is the legacy alias for full mirroring"
#endif

#if HIGH_FIRST != 0 && HIGH_FIRST != 1
#error "HIGH_FIRST must be either 0 or 1"
#endif

#if PERM_ONLY != 0 && PERM_ONLY != 1
#error "PERM_ONLY must be either 0 or 1"
#endif

#if ADDITIVE_PERM_LINEAR != 0 && ADDITIVE_PERM_LINEAR != 1
#error "ADDITIVE_PERM_LINEAR must be either 0 or 1"
#endif

#if ROW2_MITM != 0 && ROW2_MITM != 1
#error "ROW2_MITM must be either 0 or 1"
#endif

#if ROW1_MITM != 0 && ROW1_MITM != 1
#error "ROW1_MITM must be either 0 or 1"
#endif

#if CONDITIONED_COMPLETION_BOUNDS != 0 && \
    CONDITIONED_COMPLETION_BOUNDS != 1
#error "CONDITIONED_COMPLETION_BOUNDS must be either 0 or 1"
#endif

#if EARLY_CONDITIONED_COMPLETION_BOUNDS != 0 && \
    EARLY_CONDITIONED_COMPLETION_BOUNDS != 1
#error "EARLY_CONDITIONED_COMPLETION_BOUNDS must be either 0 or 1"
#endif

#if INCREMENTAL_C_COMPLETION != 0 && INCREMENTAL_C_COMPLETION != 1
#error "INCREMENTAL_C_COMPLETION must be either 0 or 1"
#endif

#if INCREMENTAL_C_COMPLETION_DIRECTIONS < 1 || \
    INCREMENTAL_C_COMPLETION_DIRECTIONS > 6
#error "C_COMPLETION_DIRECTIONS must be between one and six"
#endif

#if INCREMENTAL_C_COMPLETION_COUPLED != 0 && \
    INCREMENTAL_C_COMPLETION_COUPLED != 1
#error "C_COMPLETION_COUPLED must be either zero or one"
#endif

#if ROW2_MITM && \
    ((DICE != 4 && DICE != 5) || !PERM_ONLY || SEARCH_COLUMNS > 30)
#error "ROW2_MITM requires DICE=4..5 PERM_ONLY=1 and at most 30 independent columns"
#endif

#if ROW1_MITM && \
    (DICE != 4 || !PERM_ONLY || !ROW2_MITM)
#error "ROW1_MITM requires ROW2_MITM=1 and DICE=4 PERM_ONLY=1"
#endif

#if CONDITIONED_COMPLETION_BOUNDS && \
    ((DICE != 4 && DICE != 5) || !PERM_ONLY || !ROW2_MITM)
#error "COMPLETION_BOUNDS requires DICE=4..5 PERM_ONLY=1 ROW_MITM=1"
#endif

#if INCREMENTAL_C_COMPLETION && \
    (DICE != 5 || !CONDITIONED_COMPLETION_BOUNDS || !ROW2_MITM || !PERM_ONLY)
#error "C_COMPLETION requires DICE=5 PERM_ONLY=1 ROW_MITM=1 COMPLETION_BOUNDS=1"
#endif

#define ADDITIVE_PERM_BOUNDS_ACTIVE (PERM_ONLY && DICE >= 4)
#define ADDITIVE_PERM_LINEAR_ACTIVE \
    (ADDITIVE_PERM_BOUNDS_ACTIVE && ADDITIVE_PERM_LINEAR)
#define ROW2_MITM_ACTIVE \
    (ROW2_MITM && (DICE == 4 || DICE == 5) && PERM_ONLY && \
     SEARCH_COLUMNS <= 30)
#define ROW1_MITM_ACTIVE \
    (ROW1_MITM && ROW2_MITM_ACTIVE && DICE == 4 && SEARCH_COLUMNS <= 24)
#define CONDITIONED_COMPLETION_BOUNDS_ACTIVE \
    (CONDITIONED_COMPLETION_BOUNDS && ROW2_MITM_ACTIVE && \
     (DICE == 4 || DICE == 5))
#define EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE \
    (CONDITIONED_COMPLETION_BOUNDS_ACTIVE && DICE == 5 && \
     EARLY_CONDITIONED_COMPLETION_BOUNDS)
#define INCREMENTAL_C_COMPLETION_ACTIVE \
    (CONDITIONED_COMPLETION_BOUNDS_ACTIVE && DICE == 5 && \
     INCREMENTAL_C_COMPLETION)

#if MIRROR && ((SIDES % 2) != 0)
#error "Mirrored column-grouped dice require an even SIDES value"
#endif

#if !FULL_MIRROR && SIDES <= 255
#define PACKED_PAIR_WINS 1
#else
#define PACKED_PAIR_WINS 0
#endif

#define PLACE_DIRECTION_COUNT ((DICE * (DICE - 1)) / 2)
#if ROW2_MITM_ACTIVE
#define ROW2_MITM_MAX_LEFT_VARIABLES ((SEARCH_COLUMNS + 1U) / 2U)
#define ROW2_MITM_LEFT_CAPACITY (1U << ROW2_MITM_MAX_LEFT_VARIABLES)
#define ROW2_MITM_HASH_CAPACITY (2U * ROW2_MITM_LEFT_CAPACITY)
#if DICE == 4
#define ROW2_MITM_VECTOR_WIDTH 8U
#else
#define ROW2_MITM_VECTOR_WIDTH 27U
#endif
#endif
#if ROW1_MITM_ACTIVE
#if SEARCH_COLUMNS <= 18
#define ROW1_MITM_LEFT_CAPACITY 6561U /* 3^8 for noncanonical 4d18 */
#define ROW1_MITM_HASH_CAPACITY 16384U
#else
#define ROW1_MITM_LEFT_CAPACITY 177147U /* 3^11 for noncanonical 4d24 */
#define ROW1_MITM_HASH_CAPACITY 524288U
#endif
#if FULL_MIRROR
#define ROW1_MITM_VECTOR_WIDTH 3U
#else
#define ROW1_MITM_VECTOR_WIDTH 4U
#endif
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
#if DICE == 4
#define CONDITIONED_DIRECTION_COUNT 7U
#define COLUMN_PERMUTATION_COUNT 24U
#else
#define CONDITIONED_DIRECTION_COUNT 23U
#define COLUMN_PERMUTATION_COUNT 120U
#endif
#endif
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE || \
    INCREMENTAL_C_COMPLETION_ACTIVE
#define EARLY_COMPLETION_DIRECTION_COUNT 6U
#define EARLY_COMPLETION_G_LIMIT ((SIDES * (SIDES + 1U)) / 2U)
#define EARLY_COMPLETION_STATE_COUNT (2U * EARLY_COMPLETION_G_LIMIT + 1U)
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
/*
 * CDE and CED make G deterministic; the other four suffix orders need a
 * sparse G frontier.  The canonical column fixes the remaining D/E order,
 * so all six directions can contribute distinct pruning.
 */
#define C_COMPLETION_BASE_SCALAR_DIRECTION_COUNT 2U
#define C_COMPLETION_COUPLED_DIRECTION_COUNT \
    (INCREMENTAL_C_COMPLETION_COUPLED ? 2U : 0U)
/* X+Y and X-Y require both scalar base identities. */
#define C_COMPLETION_ACTIVE_BASE_DIRECTION_COUNT \
    (INCREMENTAL_C_COMPLETION_COUPLED && \
         INCREMENTAL_C_COMPLETION_DIRECTIONS < 2 \
     ? 2U : INCREMENTAL_C_COMPLETION_DIRECTIONS)
#define C_COMPLETION_ACTIVE_BASE_SCALAR_DIRECTION_COUNT \
    (C_COMPLETION_ACTIVE_BASE_DIRECTION_COUNT < \
         C_COMPLETION_BASE_SCALAR_DIRECTION_COUNT \
     ? C_COMPLETION_ACTIVE_BASE_DIRECTION_COUNT \
     : C_COMPLETION_BASE_SCALAR_DIRECTION_COUNT)
#define C_COMPLETION_ACTIVE_SCALAR_DIRECTION_COUNT \
    (C_COMPLETION_ACTIVE_BASE_SCALAR_DIRECTION_COUNT + \
     C_COMPLETION_COUPLED_DIRECTION_COUNT)
#define C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT \
    (C_COMPLETION_ACTIVE_BASE_DIRECTION_COUNT > \
         C_COMPLETION_BASE_SCALAR_DIRECTION_COUNT \
     ? C_COMPLETION_ACTIVE_BASE_DIRECTION_COUNT - \
           C_COMPLETION_BASE_SCALAR_DIRECTION_COUNT \
     : 0U)
#define C_COMPLETION_ACTIVE_DIRECTION_COUNT \
    (C_COMPLETION_ACTIVE_SCALAR_DIRECTION_COUNT + \
     C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT)
#if INCREMENTAL_C_COMPLETION_COUPLED
#define C_COMPLETION_PLUS_DIRECTION \
    C_COMPLETION_ACTIVE_BASE_SCALAR_DIRECTION_COUNT
#define C_COMPLETION_MINUS_DIRECTION (C_COMPLETION_PLUS_DIRECTION + 1U)
#endif
#endif
#define DEFAULT_PRINT_LIMIT UINT64_C(10)
#define DEFAULT_PROGRESS_SECONDS UINT64_C(1)
#define PROGRESS_CHECK_MASK UINT64_C(0x3ffff)
#define JOBS_PER_WORKER UINT64_C(8)
#define MAX_THREADS 256U
#define SOLUTION_QUEUE_CAPACITY 16384U
#define SOLUTION_FLUSH_THRESHOLD 256U
#define SOLUTION_RECORD_CAPACITY (FACE_COUNT + 128U)
#define TEMPLATE_UNASSIGNED UINT8_MAX
#ifndef PAIR_BOUND_START
#if PACKED_PAIR_WINS
#define PAIR_BOUND_START 0U
#else
#define PAIR_BOUND_START (SIDES / 2U)
#endif
#endif
#ifndef LINEAR_BOUND_START
#define LINEAR_BOUND_START 0U
#endif
#ifndef LINEAR_BOUND_STRIDE
#define LINEAR_BOUND_STRIDE 2U
#endif
#ifndef ADDITIVE_PERM_LINEAR_BOUND_STRIDE
#define ADDITIVE_PERM_LINEAR_BOUND_STRIDE 3U
#endif

/* Number of permutations and partial permutations, including the empty one. */
#if DICE == 2
#define DICE_FACTORIAL UINT64_C(2)
#define MAX_PREFIX_PERMUTATIONS 1
#define PERM_STATE_COUNT 5
#define PERM_EDGE_COUNT 2
#elif DICE == 3
#define DICE_FACTORIAL UINT64_C(6)
#define MAX_PREFIX_PERMUTATIONS 2
#define PERM_STATE_COUNT 16
#define PERM_EDGE_COUNT 5
#elif DICE == 4
#define DICE_FACTORIAL UINT64_C(24)
#define MAX_PREFIX_PERMUTATIONS 6
#define PERM_STATE_COUNT 65
#define PERM_EDGE_COUNT 16
#elif DICE == 5
#define DICE_FACTORIAL UINT64_C(120)
#define MAX_PREFIX_PERMUTATIONS 24
#define PERM_STATE_COUNT 326
#define PERM_EDGE_COUNT 65
#elif DICE == 6
#define DICE_FACTORIAL UINT64_C(720)
#define MAX_PREFIX_PERMUTATIONS 120
#define PERM_STATE_COUNT 1957
#define PERM_EDGE_COUNT 326
#else
#error "column_search supports DICE values from 2 through 6"
#endif

/*
 * Track every pair through 24 permutations.  At 120 permutations, a basis
 * comparing each tally with the first is sufficient to require equality and
 * avoids 7,140 direction checks at every node.  The maximum remains 276.
 */
#define ADDITIVE_PERM_FULL_PAIR_LIMIT 24U
#define ADDITIVE_PERM_FULL_PAIR_DIRECTIONS \
    ((ADDITIVE_PERM_FULL_PAIR_LIMIT * \
      (ADDITIVE_PERM_FULL_PAIR_LIMIT - 1U)) / 2U)
#define MAX_ADDITIVE_PERM_DIRECTIONS \
    (MAX_PREFIX_PERMUTATIONS <= ADDITIVE_PERM_FULL_PAIR_LIMIT ? \
        ((MAX_PREFIX_PERMUTATIONS * (MAX_PREFIX_PERMUTATIONS - 1U)) / 2U) : \
        (MAX_PREFIX_PERMUTATIONS - 1U > \
             ADDITIVE_PERM_FULL_PAIR_DIRECTIONS ? \
             MAX_PREFIX_PERMUTATIONS - 1U : \
             ADDITIVE_PERM_FULL_PAIR_DIRECTIONS))

_Static_assert(SIDES > 0, "SIDES must be positive");
_Static_assert(PAIR_BOUND_START <= SEARCH_COLUMNS,
               "PAIR_BOUND_START exceeds the searched columns");
_Static_assert(LINEAR_BOUND_START <= SEARCH_COLUMNS,
               "LINEAR_BOUND_START exceeds the searched columns");
_Static_assert(LINEAR_BOUND_STRIDE > 0,
               "LINEAR_BOUND_STRIDE must be positive");
_Static_assert(ADDITIVE_PERM_LINEAR_BOUND_STRIDE > 0,
               "ADDITIVE_PERM_LINEAR_BOUND_STRIDE must be positive");
_Static_assert(SOLUTION_FLUSH_THRESHOLD <= SOLUTION_QUEUE_CAPACITY,
               "solution flush threshold exceeds queue capacity");
_Static_assert(PERM_STATE_COUNT <= UINT16_MAX,
               "permutation state indices must fit in uint16_t");
#if ROW2_MITM_ACTIVE
_Static_assert(ROW2_MITM_VECTOR_WIDTH ==
                   MAX_PREFIX_PERMUTATIONS - 1U + DICE - 1U,
               "final-row MITM vector width is inconsistent");
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
_Static_assert(EARLY_COMPLETION_G_LIMIT <= INT16_MAX,
               "C-completion G state must fit in int16_t");
_Static_assert(EARLY_COMPLETION_STATE_COUNT <= UINT16_MAX,
               "C-completion state index must fit in uint16_t");
_Static_assert((uint64_t)SIDES * SIDES * SIDES * SIDES * SIDES <=
                   INT32_MAX,
               "C-completion tally intervals must fit in int32_t");
#endif

struct options {
    uint64_t limit;
    uint64_t jobs;
    uint64_t seed;
    uint64_t print_limit;
    uint64_t progress_seconds;
    unsigned threads;
    const char *template_path;
    const char *solutions_path;
    uint8_t template_value[DICE][SIDES];
    unsigned template_lock_count;
    bool random_order;
    bool seed_given;
    bool template_active;
    bool quiet;
};

struct perm_state {
    uint64_t key;
    unsigned mask;
    unsigned length;
};

struct perm_edge {
    uint16_t source;
    uint16_t destination;
};

struct perm_counter {
    struct perm_state state[PERM_STATE_COUNT];
    struct perm_edge edge[DICE][PERM_EDGE_COUNT];
    uint64_t ways[PERM_STATE_COUNT];
    size_t length_begin[DICE + 2];
};

#if ADDITIVE_PERM_BOUNDS_ACTIVE
/*
 * When row r is being built, dice [0,r) are fixed.  Each candidate face's
 * contribution to every ordering of dice [0,r] is therefore independent of
 * the other faces selected for row r and can be added and removed directly.
 */
struct additive_perm_bounds {
    uint16_t prefix_state[DICE][MAX_PREFIX_PERMUTATIONS];
    uint16_t reverse_suffix_state[DICE][MAX_PREFIX_PERMUTATIONS];
    unsigned permutation_count[DICE];
    uint64_t goal[DICE];

    uint64_t contribution[DICE][FACE_COUNT][MAX_PREFIX_PERMUTATIONS];
    uint64_t tally[DICE][MAX_PREFIX_PERMUTATIONS];
    uint64_t minimum_left[DICE][SEARCH_COLUMNS + 1]
                         [MAX_PREFIX_PERMUTATIONS];
    uint64_t maximum_left[DICE][SEARCH_COLUMNS + 1]
                         [MAX_PREFIX_PERMUTATIONS];
#if ADDITIVE_PERM_LINEAR_ACTIVE
    uint16_t direction_first[DICE][MAX_ADDITIVE_PERM_DIRECTIONS];
    uint16_t direction_second[DICE][MAX_ADDITIVE_PERM_DIRECTIONS];
    unsigned direction_count[DICE];
    bool linear_possible[DICE];
    int64_t direction_minimum_left[DICE][SEARCH_COLUMNS + 1]
                                  [MAX_ADDITIVE_PERM_DIRECTIONS];
    int64_t direction_maximum_left[DICE][SEARCH_COLUMNS + 1]
                                  [MAX_ADDITIVE_PERM_DIRECTIONS];
#endif

    /* Reused only while one row's contribution table is constructed. */
    uint64_t above[FACE_COUNT][MAX_PREFIX_PERMUTATIONS];
    uint64_t lower_ways[PERM_STATE_COUNT];
    uint64_t reverse_ways[PERM_STATE_COUNT];
};
#endif

struct shared_state;
struct worker_stats;

#if ROW2_MITM_ACTIVE
struct row2_mitm_entry {
    uint64_t hash;
    uint32_t choices;
    uint32_t next;
};
#endif

#if ROW1_MITM_ACTIVE
struct row1_mitm_entry {
    uint64_t hash;
    uint32_t choices;
    uint32_t next;
};
#endif

#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
struct conditioned_completion_bounds {
    uint8_t positions[COLUMN_PERMUTATION_COUNT][DICE];
    uint8_t word[CONDITIONED_DIRECTION_COUNT + 1U][DICE];
#if DICE == 4
    int16_t unary[CONDITIONED_DIRECTION_COUNT][SIDES]
                 [COLUMN_PERMUTATION_COUNT];
#endif
};
#endif

#if INCREMENTAL_C_COMPLETION_ACTIVE
struct incremental_c_transition {
    int32_t local;
    int16_t g;
    int8_t y;
    uint8_t d_position;
};

struct incremental_c_completion {
    struct incremental_c_transition
        transition[C_COMPLETION_ACTIVE_DIRECTION_COUNT][SIDES]
                  [DICE][2];
    uint8_t available_c_position[SIDES][DICE - 2U];
    int32_t suffix_minimum[C_COMPLETION_ACTIVE_DIRECTION_COUNT][SIDES + 1U]
                          [EARLY_COMPLETION_STATE_COUNT];
    int32_t suffix_maximum[C_COMPLETION_ACTIVE_DIRECTION_COUNT][SIDES + 1U]
                          [EARLY_COMPLETION_STATE_COUNT];
    int32_t prefix_minimum[C_COMPLETION_ACTIVE_SCALAR_DIRECTION_COUNT]
                          [SEARCH_COLUMNS + 1U];
    int32_t prefix_maximum[C_COMPLETION_ACTIVE_SCALAR_DIRECTION_COUNT]
                          [SEARCH_COLUMNS + 1U];
    int16_t prefix_g[C_COMPLETION_ACTIVE_SCALAR_DIRECTION_COUNT]
                    [SEARCH_COLUMNS + 1U];
#if C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT > 0
    int32_t frontier_minimum[C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT]
                            [SEARCH_COLUMNS + 1U]
                            [EARLY_COMPLETION_STATE_COUNT];
    int32_t frontier_maximum[C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT]
                            [SEARCH_COLUMNS + 1U]
                            [EARLY_COMPLETION_STATE_COUNT];
    uint16_t frontier_state[C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT]
                           [SEARCH_COLUMNS + 1U]
                           [EARLY_COMPLETION_STATE_COUNT];
    uint16_t frontier_count[C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT]
                           [SEARCH_COLUMNS + 1U];
    bool frontiers_initialized;
#endif
};
#endif

struct search {
    /* Face labels are zero based internally. */
    unsigned grid[DICE][SIDES];
    unsigned owner[FACE_COUNT];

    /* Internal build order may differ from the user-facing A, B, ... order. */
    unsigned logical_die[DICE];
    bool fixed_cell[DICE][SEARCH_COLUMNS];
    unsigned fixed_row_count[DICE];
    unsigned fixed_column_count[SEARCH_COLUMNS];
    unsigned candidate_mask[DICE][SEARCH_COLUMNS];
    bool template_active;
    bool template_possible;

    /* Recursion-depth to physical-column mapping, planned once per row. */
    unsigned column_order[DICE][SEARCH_COLUMNS];

    /* contribution[face][place], where place zero is the highest roll. */
    uint64_t contribution[FACE_COUNT][DICE];
    uint64_t tally[DICE][DICE];

    /* Safe suffix bounds for the die currently being filled. */
    uint64_t minimum_left[DICE][SEARCH_COLUMNS + 1][DICE];
    uint64_t maximum_left[DICE][SEARCH_COLUMNS + 1][DICE];
    int64_t direction_minimum_left[DICE][SEARCH_COLUMNS + 1]
                                  [PLACE_DIRECTION_COUNT];
    int64_t direction_maximum_left[DICE][SEARCH_COLUMNS + 1]
                                  [PLACE_DIRECTION_COUNT];
#if ROW2_MITM_ACTIVE
    struct row2_mitm_entry row2_mitm_entry[ROW2_MITM_LEFT_CAPACITY];
    uint32_t row2_mitm_head[ROW2_MITM_HASH_CAPACITY];
#endif
#if ROW1_MITM_ACTIVE
    struct row1_mitm_entry row1_mitm_entry[ROW1_MITM_LEFT_CAPACITY];
    uint32_t row1_mitm_head[ROW1_MITM_HASH_CAPACITY];
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    struct conditioned_completion_bounds completion_bounds;
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
    struct incremental_c_completion c_completion;
#endif
#if PACKED_PAIR_WINS
    /* One byte-wide win counter per previous die, packed into a word. */
    uint64_t pair_wins[DICE];
    uint64_t pair_increment[DICE][SEARCH_COLUMNS][DICE];
    uint64_t pair_minimum_left[DICE][SEARCH_COLUMNS + 1];
    uint64_t pair_maximum_left[DICE][SEARCH_COLUMNS + 1];
#elif !FULL_MIRROR
    unsigned pair_wins[DICE][DICE];
#endif

    uint64_t outcome_count;
    uint64_t place_goal;
    bool permutation_fairness_possible;
    bool linear_place_bounds_possible;

    struct perm_counter permutations;
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    struct additive_perm_bounds additive_permutations;
#endif
    struct shared_state *shared;
    struct worker_stats *published;

    uint64_t nodes;
#if ROW2_MITM_ACTIVE
    uint64_t mitm_solves;
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    uint64_t completion_checks;
    uint64_t completion_prunes;
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    uint64_t early_completion_checks;
    uint64_t early_completion_prunes;
    uint64_t early_completion_states;
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
    uint64_t c_completion_checks;
    uint64_t c_completion_prunes;
#if INCREMENTAL_C_COMPLETION_COUPLED
    uint64_t c_completion_coupled_prunes;
#endif
    uint64_t c_completion_states;
#endif
#endif
    uint64_t bound_prunes;
    uint64_t linear_place_prunes;
    uint64_t pair_bound_prunes;
    uint64_t prefix_place_prunes;
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    uint64_t additive_perm_prunes;
#if ADDITIVE_PERM_LINEAR_ACTIVE
    uint64_t linear_perm_prunes;
#endif
#endif
    uint64_t all_subset_place_prunes;
    uint64_t all_subset_place_fair_count;
    uint64_t permutation_fair_count;
};

struct worker_stats {
    atomic_uint_fast64_t nodes;
#if ROW2_MITM_ACTIVE
    atomic_uint_fast64_t mitm_solves;
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    atomic_uint_fast64_t completion_checks;
    atomic_uint_fast64_t completion_prunes;
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    atomic_uint_fast64_t early_completion_checks;
    atomic_uint_fast64_t early_completion_prunes;
    atomic_uint_fast64_t early_completion_states;
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
    atomic_uint_fast64_t c_completion_checks;
    atomic_uint_fast64_t c_completion_prunes;
#if INCREMENTAL_C_COMPLETION_COUPLED
    atomic_uint_fast64_t c_completion_coupled_prunes;
#endif
    atomic_uint_fast64_t c_completion_states;
#endif
#endif
    atomic_uint_fast64_t bound_prunes;
    atomic_uint_fast64_t linear_place_prunes;
    atomic_uint_fast64_t pair_bound_prunes;
    atomic_uint_fast64_t prefix_place_prunes;
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    atomic_uint_fast64_t additive_perm_prunes;
#if ADDITIVE_PERM_LINEAR_ACTIVE
    atomic_uint_fast64_t linear_perm_prunes;
#endif
#endif
    atomic_uint_fast64_t all_subset_place_prunes;
    atomic_uint_fast64_t all_subset_place_fair_count;
    atomic_uint_fast64_t permutation_fair_count;
};

enum solution_kind {
    ALL_SUBSET_PLACE_FAIR,
    PERMUTATION_FAIR,
};

struct solution {
    enum solution_kind kind;
    uint64_t number;
    char encoding[FACE_COUNT + 1];
};

struct shared_state {
    struct options options;
    unsigned thread_count;
    unsigned start_row;
    unsigned prefix_columns;
    unsigned split_depth;
    uint64_t job_count;
    uint64_t prefix_count;
    uint64_t *job_order;

    atomic_uint_fast64_t next_job;
    atomic_uint_fast64_t jobs_done;
    atomic_uint_fast64_t limit_claims;
    atomic_uint_fast64_t all_subset_total;
    atomic_uint_fast64_t permutation_total;
    /* Written only by the main watcher while draining solution batches. */
    uint64_t mirror_symmetric_total;
    atomic_bool stop;
    atomic_bool internal_error;

    pthread_mutex_t completion_mutex;
    pthread_cond_t completion_condition;
    unsigned workers_running;

    pthread_mutex_t solution_mutex;
    pthread_cond_t solution_not_full;
    struct solution *solution_queue;
    struct solution *solution_batch;
    char *solution_write_buffer;
    unsigned solution_count;
    bool solution_file_failed;
};

struct worker {
    pthread_t thread;
    unsigned id;
    struct shared_state *shared;
    struct worker_stats stats;
    struct search search;
};

static volatile sig_atomic_t sigint_requested;

static void request_sigint_stop(int signal_number)
{
    (void)signal_number;
    sigint_requested = 1;
}

static bool install_sigint_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = request_sigint_stop;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0) {
        fprintf(stderr, "Unable to install Ctrl-C handler: %s\n",
                strerror(errno));
        return false;
    }
    return true;
}

static const char *traversal_description(void)
{
#if INCREMENTAL_C_COMPLETION_ACTIVE
    return "fail-first A/B; physical C with incremental completion; exact MITM D";
#elif EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    return "fail-first; conditioned A/B and final-row bounds; exact MITM final built die";
#elif CONDITIONED_COMPLETION_BOUNDS_ACTIVE && ROW1_MITM_ACTIVE
    return "fail-first first die; conditioned completion bounds; exact MITM final built dice";
#elif CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    return "fail-first; conditioned completion bounds; exact MITM final built die";
#elif ROW1_MITM_ACTIVE
    return "fail-first first die; exact MITM final built dice";
#elif ROW2_MITM_ACTIVE
    return "fail-first; exact MITM final built die";
#elif MIRROR_COLUMNS > 0
    return "fail-first; outer-pair ties first";
#elif HIGH_FIRST
    return "fail-first; high-label ties first";
#else
    return "fail-first; low-label ties first";
#endif
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [OPTIONS]\n"
            "\n"
            "Search the compile-time-selected %s%dd%d column-grouped space.\n"
            "Without a template, the first searched column removes equivalent "
            "die renamings.\n"
            "Reported encodings are relabeled canonically so their first "
            "column is in ABC... order.\n"
#if PERM_ONLY
            "This build searches only for permutation-fair results and uses "
            "incremental prefix-permutation bounds.\n"
#else
            "Results are place-fair for every subset. Fully permutation-fair "
            "results are reported only in the stronger class.\n"
#endif
            "\n"
            "  -t, --threads N     worker threads; default is online CPUs\n"
            "  -j, --jobs N        logical jobs; default is chosen automatically\n"
            "      --template FILE lock selected die/column assignments\n"
            "                      first row may start '0 1 ...' or 'columns:';\n"
            "                      it must list 0 through %d exactly, in order;\n"
            "                      rows use 'A:' plus exactly SIDES entries;\n"
            "                      values are offsets, with '.' or 'x' free\n"
            "      --solutions-file FILE\n"
            "                      append every result with file locking\n"
            "      --random-order shuffle jobs before starting workers\n"
            "      --seed N        random-order seed; requires --random-order\n"
#if PERM_ONLY
            "  -n, --limit N       stop after N permutation-fair results\n"
#else
            "  -n, --limit N       stop after N reported results\n"
#endif
            "  -p, --progress N    progress interval in seconds; 0 disables\n"
#if PERM_ONLY
            "      --print-limit N print at most N solutions\n"
            "      --all-solutions print every solution\n"
#else
            "      --print-limit N print at most N of each solution class\n"
            "      --all-solutions print every solution in both classes\n"
#endif
            "  -q, --quiet         print only startup and final counts\n"
            "  -h, --help          show this help\n",
            program,
            FULL_MIRROR ? "mirrored " :
                (MIRROR_COLUMNS > 0 ? "partially mirrored " : ""),
            DICE, SIDES, SIDES - 1);
}

static bool parse_uint64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (*text == '-') {
        return false;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static bool template_keyword_is(const char *text, const char *keyword)
{
    while (*keyword != '\0') {
        if (tolower((unsigned char)*text) !=
            tolower((unsigned char)*keyword)) {
            return false;
        }
        ++text;
        ++keyword;
    }
    return *text == ':' || isspace((unsigned char)*text);
}

static bool parse_template_column_header(const struct options *options,
                                         char *cursor,
                                         unsigned line_number)
{
    unsigned column;

    for (column = 0; column < SIDES; ++column) {
        char *end;
        unsigned long value;

        while (isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0' || *cursor == '#') {
            fprintf(stderr,
                    "%s:%u: column header has %u values; this build "
                    "requires exactly %d.\n",
                    options->template_path, line_number, column, SIDES);
            return false;
        }
        if (!isdigit((unsigned char)*cursor)) {
            fprintf(stderr,
                    "%s:%u: column header entry %u must be %u.\n",
                    options->template_path, line_number, column, column);
            return false;
        }
        errno = 0;
        value = strtoul(cursor, &end, 10);
        if (errno != 0 || end == cursor || value != column ||
            (*end != '\0' && *end != '#' &&
             !isspace((unsigned char)*end))) {
            fprintf(stderr,
                    "%s:%u: column header entry %u must be %u.\n",
                    options->template_path, line_number, column, column);
            return false;
        }
        cursor = end;
    }
    while (isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor != '\0' && *cursor != '#') {
        fprintf(stderr,
                "%s:%u: column header has more than %d values.\n",
                options->template_path, line_number, SIDES);
        return false;
    }
    return true;
}

static bool parse_template_file(struct options *options)
{
    FILE *file = fopen(options->template_path, "r");
    char *line = NULL;
    size_t capacity = 0;
    unsigned line_number = 0;
    bool seen[DICE] = {false};
    bool saw_column_header = false;
    bool saw_die_row = false;
    bool valid = true;

    if (file == NULL) {
        fprintf(stderr, "Unable to open template '%s': %s\n",
                options->template_path, strerror(errno));
        return false;
    }
    while (getline(&line, &capacity, file) >= 0) {
        char *cursor = line;
        unsigned logical_die;
        unsigned column;

        ++line_number;
        while (isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0' || *cursor == '#') {
            continue;
        }
        if (template_keyword_is(cursor, "columns") ||
            isdigit((unsigned char)*cursor)) {
            bool named_header = template_keyword_is(cursor, "columns");

            if (saw_column_header || saw_die_row) {
                fprintf(stderr,
                        "%s:%u: the column header must appear once, before "
                        "all die rows.\n",
                        options->template_path, line_number);
                valid = false;
                break;
            }
            if (named_header) {
                cursor += sizeof("columns") - 1U;
                while (isspace((unsigned char)*cursor)) {
                    ++cursor;
                }
                if (*cursor != ':') {
                    fprintf(stderr,
                            "%s:%u: expected ':' after 'columns'.\n",
                            options->template_path, line_number);
                    valid = false;
                    break;
                }
                ++cursor;
            }
            if (!parse_template_column_header(options, cursor,
                                              line_number)) {
                valid = false;
                break;
            }
            saw_column_header = true;
            continue;
        }
        if (isalpha((unsigned char)*cursor)) {
            int label = toupper((unsigned char)*cursor);

            logical_die = (unsigned)(label - 'A');
        } else {
            logical_die = DICE;
        }
        ++cursor;
        while (isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if (logical_die >= DICE || *cursor != ':') {
            fprintf(stderr,
                    "%s:%u: expected a die label from A through %c followed "
                    "by ':'.\n",
                    options->template_path, line_number, 'A' + DICE - 1);
            valid = false;
            break;
        }
        saw_die_row = true;
        if (seen[logical_die]) {
            fprintf(stderr, "%s:%u: die %c appears more than once.\n",
                    options->template_path, line_number,
                    'A' + logical_die);
            valid = false;
            break;
        }
        seen[logical_die] = true;
        ++cursor;

        for (column = 0; column < SIDES; ++column) {
            char *end;
            unsigned long value;

            while (isspace((unsigned char)*cursor)) {
                ++cursor;
            }
            if (*cursor == '\0' || *cursor == '#') {
                fprintf(stderr,
                        "%s:%u: die %c has %u values; this build requires "
                        "exactly %d.\n",
                        options->template_path, line_number,
                        'A' + logical_die, column, SIDES);
                valid = false;
                break;
            }
            if ((*cursor == '.' || *cursor == 'x' || *cursor == 'X') &&
                (cursor[1] == '\0' || cursor[1] == '#' ||
                 isspace((unsigned char)cursor[1]))) {
                ++cursor;
                continue;
            }
            errno = 0;
            value = strtoul(cursor, &end, 10);
            if (errno != 0 || end == cursor || value >= DICE ||
                (*end != '\0' && *end != '#' &&
                 !isspace((unsigned char)*end))) {
                fprintf(stderr,
                        "%s:%u: column %u must be '.', 'x', or a value from "
                        "0 through %d.\n",
                        options->template_path, line_number, column,
                        DICE - 1);
                valid = false;
                break;
            }
            options->template_value[logical_die][column] = (uint8_t)value;
            ++options->template_lock_count;
            cursor = end;
        }
        if (!valid) {
            break;
        }
        while (isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if (*cursor != '\0' && *cursor != '#') {
            fprintf(stderr,
                    "%s:%u: die %c has more than %d values.\n",
                    options->template_path, line_number,
                    'A' + logical_die, SIDES);
            valid = false;
            break;
        }
    }
    if (ferror(file)) {
        fprintf(stderr, "Unable to read template '%s': %s\n",
                options->template_path, strerror(errno));
        valid = false;
    }
    free(line);
    if (fclose(file) != 0 && valid) {
        fprintf(stderr, "Unable to close template '%s': %s\n",
                options->template_path, strerror(errno));
        valid = false;
    }
    if (!valid) {
        return false;
    }

    {
        unsigned column;

        for (column = 0; column < SIDES; ++column) {
            unsigned used = 0;
            unsigned die;

            for (die = 0; die < DICE; ++die) {
                uint8_t value = options->template_value[die][column];
                unsigned bit;

                if (value == TEMPLATE_UNASSIGNED) {
                    continue;
                }
                bit = 1U << value;
                if ((used & bit) != 0) {
                    fprintf(stderr,
                            "%s: column %u assigns value %u to more than "
                            "one die.\n",
                            options->template_path, column, value);
                    return false;
                }
                used |= bit;
            }
        }
    }
    if (options->template_lock_count == 0) {
        fprintf(stderr, "Template '%s' contains no fixed assignments.\n",
                options->template_path);
        return false;
    }
    options->template_active = true;
    return true;
}

static bool parse_options(int argc, char **argv, struct options *options)
{
    int i;

    *options = (struct options){
        .print_limit = DEFAULT_PRINT_LIMIT,
        .progress_seconds = DEFAULT_PROGRESS_SECONDS,
    };
    memset(options->template_value, TEMPLATE_UNASSIGNED,
           sizeof(options->template_value));
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout, argv[0]);
            exit(EXIT_SUCCESS);
        }
        if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            options->quiet = true;
            continue;
        }
        if (strcmp(argv[i], "--all-solutions") == 0) {
            options->print_limit = UINT64_MAX;
            continue;
        }
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            uint64_t threads;
            if (++i >= argc || !parse_uint64(argv[i], &threads) ||
                threads == 0 || threads > MAX_THREADS) {
                fprintf(stderr, "Thread count must be between 1 and %u.\n",
                        MAX_THREADS);
                return false;
            }
            options->threads = (unsigned)threads;
            continue;
        }
        if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--jobs") == 0) {
            if (++i >= argc || !parse_uint64(argv[i], &options->jobs) ||
                options->jobs == 0) {
                fprintf(stderr, "Job count must be positive.\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[i], "--random-order") == 0) {
            options->random_order = true;
            continue;
        }
        if (strcmp(argv[i], "--template") == 0) {
            if (++i >= argc || options->template_path != NULL) {
                fprintf(stderr, "--template requires one template file.\n");
                return false;
            }
            options->template_path = argv[i];
            continue;
        }
        if (strcmp(argv[i], "--solutions-file") == 0) {
            if (++i >= argc || options->solutions_path != NULL ||
                *argv[i] == '\0') {
                fprintf(stderr,
                        "--solutions-file requires one non-empty file path.\n");
                return false;
            }
            options->solutions_path = argv[i];
            continue;
        }
        if (strcmp(argv[i], "--seed") == 0) {
            if (++i >= argc || !parse_uint64(argv[i], &options->seed)) {
                fprintf(stderr, "Invalid random-order seed.\n");
                return false;
            }
            options->seed_given = true;
            continue;
        }
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--limit") == 0) {
            if (++i >= argc || !parse_uint64(argv[i], &options->limit)) {
                fprintf(stderr, "Invalid solution limit.\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--progress") == 0) {
            if (++i >= argc ||
                !parse_uint64(argv[i], &options->progress_seconds)) {
                fprintf(stderr, "Invalid progress interval.\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[i], "--print-limit") == 0) {
            if (++i >= argc || !parse_uint64(argv[i], &options->print_limit)) {
                fprintf(stderr, "Invalid print limit.\n");
                return false;
            }
            continue;
        }
        fprintf(stderr, "Unknown argument: %s\n", argv[i]);
        return false;
    }
    if (options->quiet) {
        options->print_limit = 0;
        options->progress_seconds = 0;
    }
    if (options->seed_given && !options->random_order) {
        fprintf(stderr, "--seed requires --random-order.\n");
        return false;
    }
    if (options->template_path != NULL && !parse_template_file(options)) {
        return false;
    }
    return true;
}

static uint64_t default_random_seed(void)
{
    struct timespec realtime = {0};
    struct timespec monotonic = {0};
    uint64_t seed;

    (void)clock_gettime(CLOCK_REALTIME, &realtime);
    (void)clock_gettime(CLOCK_MONOTONIC, &monotonic);
    seed = (uint64_t)realtime.tv_sec ^
        ((uint64_t)realtime.tv_nsec << 32U) ^
        (uint64_t)monotonic.tv_nsec ^ (uint64_t)getpid();
    return seed;
}

/* SplitMix64 is small, fast, and sufficient for shuffling independent jobs. */
static uint64_t next_job_random(uint64_t *state)
{
    uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));

    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static uint64_t random_below(uint64_t *state, uint64_t bound)
{
    uint64_t threshold = (UINT64_C(0) - bound) % bound;
    uint64_t value;

    do {
        value = next_job_random(state);
    } while (value < threshold);
    return value % bound;
}

static bool initialize_job_order(struct shared_state *shared)
{
    uint64_t state;
    uint64_t i;

    if (!shared->options.random_order) {
        return true;
    }
    if (shared->job_count > SIZE_MAX / sizeof(*shared->job_order)) {
        fprintf(stderr, "Random job-order table is too large to allocate.\n");
        return false;
    }
    shared->job_order = malloc(
        (size_t)shared->job_count * sizeof(*shared->job_order));
    if (shared->job_order == NULL) {
        fprintf(stderr, "Unable to allocate random job-order table.\n");
        return false;
    }
    for (i = 0; i < shared->job_count; ++i) {
        shared->job_order[i] = i;
    }

    state = shared->options.seed;
    for (i = shared->job_count; i > 1U; --i) {
        uint64_t selected = random_below(&state, i);
        uint64_t temporary = shared->job_order[i - 1U];

        shared->job_order[i - 1U] = shared->job_order[selected];
        shared->job_order[selected] = temporary;
    }
    return true;
}

static bool integer_power(uint64_t base, unsigned exponent, uint64_t *result)
{
    uint64_t value = 1;
    unsigned i;

    for (i = 0; i < exponent; ++i) {
        if (base != 0 && value > UINT64_MAX / base) {
            return false;
        }
        value *= base;
    }
    *result = value;
    return true;
}

/*
 * Build the coefficient of x^place in
 *
 *     product(other die) (faces_below + faces_above * x).
 *
 * For a face in column c at offset r, r other same-column faces are lower
 * and DICE-1-r are higher.  Column grouping therefore supplies all below and
 * above counts without knowing which particular dice own those faces.
 */
static bool build_contributions(struct search *search)
{
    uint64_t fixed_face_outcomes;
    unsigned face;

    if (!integer_power(SIDES, DICE - 1, &fixed_face_outcomes)) {
        return false;
    }

    for (face = 0; face < FACE_COUNT; ++face) {
        uint64_t polynomial[DICE + 1] = {0};
        unsigned column = face / DICE;
        unsigned offset = face % DICE;
        unsigned degree = 0;
        unsigned opponent;

        polynomial[0] = 1;
        for (opponent = 0; opponent < DICE - 1; ++opponent) {
            uint64_t next[DICE + 1] = {0};
            uint64_t below;
            uint64_t above;
            unsigned place;

            if (opponent < offset) {
                below = column + 1U;
                above = SIDES - column - 1U;
            } else {
                below = column;
                above = SIDES - column;
            }

            for (place = 0; place <= degree; ++place) {
                next[place] += polynomial[place] * below;
                next[place + 1U] += polynomial[place] * above;
            }
            memcpy(polynomial, next, sizeof(polynomial));
            ++degree;
        }

        {
            uint64_t sum = 0;
            unsigned place;
            for (place = 0; place < DICE; ++place) {
                search->contribution[face][place] = polynomial[place];
                sum += polynomial[place];
            }
            if (sum != fixed_face_outcomes) {
                return false;
            }
        }
    }

    /* Across all dice, every complete roll has one occupant of each place. */
    {
        unsigned place;
        for (place = 0; place < DICE; ++place) {
            uint64_t sum = 0;
            for (face = 0; face < FACE_COUNT; ++face) {
                sum += search->contribution[face][place];
            }
            if (sum != search->outcome_count) {
                return false;
            }
        }
    }
    return true;
}


static void generate_perm_states(struct perm_counter *counter,
                                 unsigned target_length, unsigned depth,
                                 unsigned mask, uint64_t key, size_t *next)
{
    unsigned die;

    if (depth == target_length) {
        counter->state[*next] = (struct perm_state){
            .key = key,
            .mask = mask,
            .length = depth,
        };
        ++*next;
        return;
    }

    for (die = 0; die < DICE; ++die) {
        unsigned bit = 1U << die;
        if ((mask & bit) == 0) {
            uint64_t digit = (uint64_t)(die + 1U) << (4U * depth);
            generate_perm_states(counter, target_length, depth + 1U,
                                 mask | bit, key | digit, next);
        }
    }
}

static int find_perm_state(const struct perm_counter *counter,
                           unsigned length, uint64_t key)
{
    size_t i;

    for (i = counter->length_begin[length];
         i < counter->length_begin[length + 1U]; ++i) {
        if (counter->state[i].key == key) {
            return (int)i;
        }
    }
    return -1;
}

static bool initialize_perm_counter(struct perm_counter *counter)
{
    size_t next = 0;
    unsigned length;

    for (length = 0; length <= DICE; ++length) {
        counter->length_begin[length] = next;
        generate_perm_states(counter, length, 0, 0, 0, &next);
    }
    counter->length_begin[DICE + 1] = next;
    if (next != PERM_STATE_COUNT) {
        return false;
    }

    {
        unsigned die;
        for (die = 0; die < DICE; ++die) {
            size_t edge_count = 0;
            next = PERM_STATE_COUNT;
            while (next-- > 0) {
                const struct perm_state *state = &counter->state[next];
                int destination;
                uint64_t key = state->key |
                    ((uint64_t)(die + 1U) << (4U * state->length));

                if ((state->mask & (1U << die)) != 0 ||
                    state->length == DICE) {
                    continue;
                }
                destination = find_perm_state(
                    counter, state->length + 1U, key);
                if (destination < 0 || edge_count >= PERM_EDGE_COUNT) {
                    return false;
                }
                counter->edge[die][edge_count] = (struct perm_edge){
                    .source = (uint16_t)next,
                    .destination = (uint16_t)destination,
                };
                ++edge_count;
            }
            if (edge_count != PERM_EDGE_COUNT) {
                return false;
            }
        }
    }
    return true;
}

static void build_owner_table(struct search *search)
{
    unsigned die;

    for (die = 0; die < DICE; ++die) {
        unsigned column;
        for (column = 0; column < SIDES; ++column) {
            search->owner[search->grid[die][column]] = die;
        }
    }
}

/* Count one subset's roll orderings by scanning labels from low to high. */
static void count_permutation_ways(struct search *search,
                                   unsigned subset_mask)
{
    struct perm_counter *counter = &search->permutations;
    unsigned face;

    memset(counter->ways, 0, sizeof(counter->ways));
    counter->ways[0] = 1; /* one way to choose an empty ordering */

    for (face = 0; face < FACE_COUNT; ++face) {
        unsigned owner = search->owner[face];
        size_t edge;

        if ((subset_mask & (1U << owner)) == 0) {
            continue;
        }
        /* Edges are stored by descending source for safe in-place updates. */
        for (edge = 0; edge < PERM_EDGE_COUNT; ++edge) {
            const struct perm_edge *transition = &counter->edge[owner][edge];
            counter->ways[transition->destination] +=
            counter->ways[transition->source];
        }
    }
}

static bool counted_subset_is_permutation_fair(
    const struct search *search, unsigned subset_mask, unsigned subset_size)
{
    const struct perm_counter *counter = &search->permutations;
    uint64_t subset_outcomes;
    uint64_t subset_factorial = 1;
    uint64_t goal;
    unsigned i;

    if (subset_size > DICE || subset_mask >= (1U << DICE)) {
        return false;
    }
    if (!integer_power(SIDES, subset_size, &subset_outcomes)) {
        return false;
    }
    for (i = 2; i <= subset_size; ++i) {
        subset_factorial *= i;
    }
    if (subset_outcomes % subset_factorial != 0) {
        return false;
    }
    goal = subset_outcomes / subset_factorial;

    {
        size_t state;
        for (state = counter->length_begin[subset_size];
             state < counter->length_begin[subset_size + 1U]; ++state) {
            if (counter->state[state].mask == subset_mask &&
                counter->ways[state] != goal) {
                return false;
            }
        }
    }
    return true;
}

#if !PERM_ONLY
static unsigned mask_size(unsigned mask)
{
    unsigned size = 0;

    while (mask != 0) {
        size += mask & 1U;
        mask >>= 1U;
    }
    return size;
}

/*
 * The full DP also contains every ordering count for every proper subset.
 * Sum those ordering buckets by rank to test all subset place marginals.
 * Full-set place fairness and every pair have already been established.
 */
static bool counted_all_subsets_are_place_fair(const struct search *search)
{
    uint64_t place_tally[1U << DICE][DICE][DICE] = {{{0}}};
    uint64_t goal_by_size[DICE + 1] = {0};
    const struct perm_counter *counter = &search->permutations;
    unsigned size;
    size_t state;
    unsigned mask;

    for (size = 3; size < DICE; ++size) {
        uint64_t outcomes;

        if (!integer_power(SIDES, size, &outcomes) || outcomes % size != 0) {
            return false;
        }
        goal_by_size[size] = outcomes / size;
    }

    for (state = 0; state < PERM_STATE_COUNT; ++state) {
        const struct perm_state *permutation = &counter->state[state];
        uint64_t count;
        unsigned position;

        if (permutation->length < 3U || permutation->length >= DICE) {
            continue;
        }
        count = counter->ways[state];
        for (position = 0; position < permutation->length; ++position) {
            unsigned die = (unsigned)(
                (permutation->key >> (4U * position)) & UINT64_C(0xf)) - 1U;
            place_tally[permutation->mask][die][position] += count;
        }
    }

    for (mask = 1; mask < (1U << DICE) - 1U; ++mask) {
        unsigned die;

        size = mask_size(mask);
        if (size < 3U || size >= DICE) {
            continue;
        }
        for (die = 0; die < DICE; ++die) {
            unsigned place;

            if ((mask & (1U << die)) == 0) {
                continue;
            }
            for (place = 0; place < size; ++place) {
                if (place_tally[mask][die][place] != goal_by_size[size]) {
                    return false;
                }
            }
        }
    }
    return true;
}
#endif

static unsigned physical_column(unsigned search_column);

#if MIRROR_COLUMNS > 0
static bool column_has_mirror(unsigned actual_column)
{
    return actual_column < MIRROR_COLUMNS;
}
#endif

static void initialize_grid(struct search *search)
{
    unsigned column;

    /* Alternating direction affects traversal order, not the search space. */
    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        unsigned die;

        for (die = 0; die < DICE; ++die) {
            unsigned offset = (column & 1U) == 0 ? die : DICE - die - 1U;
            unsigned face = column * DICE + offset;

            search->grid[die][column] = face;
#if MIRROR_COLUMNS > 0
            if (column_has_mirror(column)) {
                unsigned mirror_column = SIDES - column - 1U;

                search->grid[die][mirror_column] = FACE_COUNT - face - 1U;
            }
#endif
        }
    }
}

static bool configure_template(struct search *search,
                               const struct options *options)
{
    uint8_t fixed_value[DICE][SEARCH_COLUMNS];
    unsigned logical_count[DICE] = {0};
    unsigned logical;
    unsigned column;

    memset(fixed_value, TEMPLATE_UNASSIGNED, sizeof(fixed_value));
    search->template_active = options->template_active;
    search->template_possible = true;
    for (logical = 0; logical < DICE; ++logical) {
        search->logical_die[logical] = logical;
    }
    if (!options->template_active) {
        initialize_grid(search);
        return true;
    }

    for (logical = 0; logical < DICE; ++logical) {
        for (column = 0; column < SEARCH_COLUMNS; ++column) {
            unsigned actual_column = physical_column(column);
            uint8_t value = options->template_value[logical][actual_column];

#if MIRROR_COLUMNS > 0
            if (column_has_mirror(actual_column)) {
                unsigned mirror_column = SIDES - actual_column - 1U;
                uint8_t mirror_value =
                    options->template_value[logical][mirror_column];

                if (mirror_value != TEMPLATE_UNASSIGNED) {
                    uint8_t implied = (uint8_t)(DICE - 1U - mirror_value);

                    if (value != TEMPLATE_UNASSIGNED && value != implied) {
                        fprintf(stderr,
                                "Template columns %u and %u conflict for "
                                "die %c under MIRROR_COLUMNS=%u.\n",
                                actual_column, mirror_column, 'A' + logical,
                                (unsigned)MIRROR_COLUMNS);
                        return false;
                    }
                    value = implied;
                }
            }
#endif
            fixed_value[logical][actual_column] = value;
            if (value != TEMPLATE_UNASSIGNED) {
                ++logical_count[logical];
            }
        }
    }

    /* Most constrained named dice become the earliest internal search rows. */
    for (logical = 1; logical < DICE; ++logical) {
        unsigned selected = search->logical_die[logical];
        unsigned position = logical;

        while (position > 0U &&
               logical_count[search->logical_die[position - 1U]] <
                   logical_count[selected]) {
            search->logical_die[position] =
                search->logical_die[position - 1U];
            --position;
        }
        search->logical_die[position] = selected;
    }

    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        unsigned actual_column = physical_column(column);
        unsigned used = 0;
        unsigned row;

        for (row = 0; row < DICE; ++row) {
            unsigned named_die = search->logical_die[row];
            uint8_t value = fixed_value[named_die][actual_column];

            if (value == TEMPLATE_UNASSIGNED) {
                continue;
            }
            if ((used & (1U << value)) != 0) {
                fprintf(stderr,
                        "Template column %u assigns value %u to conflicting "
                        "mirrored constraints.\n",
                        actual_column, value);
                return false;
            }
            used |= 1U << value;
            search->fixed_cell[row][actual_column] = true;
            ++search->fixed_row_count[row];
            ++search->fixed_column_count[actual_column];
            search->grid[row][actual_column] =
                actual_column * DICE + value;
#if MIRROR_COLUMNS > 0
            if (column_has_mirror(actual_column)) {
                search->grid[row][SIDES - actual_column - 1U] =
                    FACE_COUNT - search->grid[row][actual_column] - 1U;
            }
#endif
        }
        for (row = 0; row < DICE; ++row) {
            unsigned offset;

            if (search->fixed_cell[row][actual_column]) {
                continue;
            }
            for (offset = 0; offset < DICE; ++offset) {
                unsigned preferred = (actual_column & 1U) == 0
                    ? offset
                    : DICE - offset - 1U;

                if ((used & (1U << preferred)) == 0) {
                    used |= 1U << preferred;
                    search->grid[row][actual_column] =
                        actual_column * DICE + preferred;
#if MIRROR_COLUMNS > 0
                    if (column_has_mirror(actual_column)) {
                        search->grid[row][SIDES - actual_column - 1U] =
                            FACE_COUNT -
                            search->grid[row][actual_column] - 1U;
                    }
#endif
                    break;
                }
            }
        }
    }
    return true;
}

/* Translate recursion depth to the physical face-label column. */
static unsigned physical_column(unsigned search_column)
{
#if MIRROR_COLUMNS > 0 || !HIGH_FIRST
    return search_column;
#else
    return SIDES - search_column - 1U;
#endif
}

static unsigned ordered_column(const struct search *search, unsigned row,
                               unsigned search_column)
{
    return search->column_order[row][search_column];
}

static void reset_column_order(struct search *search, unsigned row)
{
    unsigned column;

    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        search->column_order[row][column] = physical_column(column);
    }
    if (search->template_active) {
        for (column = 1; column < SEARCH_COLUMNS; ++column) {
            unsigned selected = search->column_order[row][column];
            unsigned position = column;

            while (position > 0U) {
                unsigned previous =
                    search->column_order[row][position - 1U];
                bool selected_fixed = search->fixed_cell[row][selected];
                bool previous_fixed = search->fixed_cell[row][previous];

                if (previous_fixed > selected_fixed ||
                    (previous_fixed == selected_fixed &&
                     search->fixed_column_count[previous] >=
                         search->fixed_column_count[selected])) {
                    break;
                }
                search->column_order[row][position] = previous;
                --position;
            }
            search->column_order[row][position] = selected;
        }
    }
}

static unsigned available_candidate_mask(const struct search *search,
                                         unsigned row,
                                         unsigned actual_column,
                                         bool canonical_column)
{
    unsigned candidate;
    unsigned mask = 0;

    if (!search->template_active) {
        if (canonical_column) {
            return 1U << row;
        }
        return ((1U << DICE) - 1U) & ~((1U << row) - 1U);
    }
    if (search->fixed_cell[row][actual_column]) {
        return 1U << row;
    }
    for (candidate = row; candidate < DICE; ++candidate) {
        if (!search->fixed_cell[candidate][actual_column]) {
            mask |= 1U << candidate;
        }
    }
    return mask;
}

#if ADDITIVE_PERM_BOUNDS_ACTIVE
static void add_owner_to_perm_ways(const struct perm_counter *counter,
                                   uint64_t ways[PERM_STATE_COUNT],
                                   unsigned owner)
{
    size_t edge;

    for (edge = 0; edge < PERM_EDGE_COUNT; ++edge) {
        const struct perm_edge *transition = &counter->edge[owner][edge];
        ways[transition->destination] += ways[transition->source];
    }
}

static bool initialize_additive_perm_bounds(struct search *search)
{
    struct additive_perm_bounds *bounds = &search->additive_permutations;
    const struct perm_counter *counter = &search->permutations;
    unsigned row;

    for (row = 2; row + 1U < DICE; ++row) {
        unsigned dice_built = row + 1U;
        unsigned prefix_mask = (1U << dice_built) - 1U;
        uint64_t outcomes;
        uint64_t factorial = 1;
        size_t state;
        unsigned permutation = 0;
        unsigned i;

        for (i = 2; i <= dice_built; ++i) {
            factorial *= i;
        }
        if (!integer_power(SIDES, dice_built, &outcomes) ||
            outcomes % factorial != 0) {
            search->permutation_fairness_possible = false;
        } else {
            bounds->goal[row] = outcomes / factorial;
#if ADDITIVE_PERM_LINEAR_ACTIVE
            bounds->linear_possible[row] = outcomes <= (uint64_t)INT64_MAX;
#endif
        }

        for (state = counter->length_begin[dice_built];
             state < counter->length_begin[dice_built + 1U]; ++state) {
            const struct perm_state *full = &counter->state[state];
            uint64_t prefix_key = 0;
            uint64_t reverse_suffix_key = 0;
            unsigned position;
            unsigned new_die_position = dice_built;
            unsigned prefix_length = 0;
            unsigned suffix_length = 0;
            int prefix_state;
            int suffix_state;

            if (full->mask != prefix_mask) {
                continue;
            }
            if (permutation >= MAX_PREFIX_PERMUTATIONS) {
                return false;
            }
            for (position = 0; position < dice_built; ++position) {
                unsigned die = (unsigned)(
                    (full->key >> (4U * position)) & UINT64_C(0xf)) - 1U;

                if (die == row) {
                    new_die_position = position;
                    break;
                }
                prefix_key |= (uint64_t)(die + 1U) << (4U * prefix_length);
                ++prefix_length;
            }
            if (new_die_position == dice_built) {
                return false;
            }
            position = dice_built;
            while (position-- > new_die_position + 1U) {
                unsigned die = (unsigned)(
                    (full->key >> (4U * position)) & UINT64_C(0xf)) - 1U;
                reverse_suffix_key |=
                    (uint64_t)(die + 1U) << (4U * suffix_length);
                ++suffix_length;
            }
            prefix_state = find_perm_state(counter, prefix_length, prefix_key);
            suffix_state = find_perm_state(
                counter, suffix_length, reverse_suffix_key);
            if (prefix_state < 0 || suffix_state < 0) {
                return false;
            }
            bounds->prefix_state[row][permutation] =
                (uint16_t)prefix_state;
            bounds->reverse_suffix_state[row][permutation] =
                (uint16_t)suffix_state;
            ++permutation;
        }
        if (permutation != factorial) {
            return false;
        }
        bounds->permutation_count[row] = permutation;
#if ADDITIVE_PERM_LINEAR_ACTIVE
        {
            unsigned direction = 0;

            if (permutation <= ADDITIVE_PERM_FULL_PAIR_LIMIT) {
                unsigned first;

                for (first = 0; first < permutation; ++first) {
                    unsigned second;

                    for (second = first + 1U; second < permutation;
                         ++second) {
                        if (direction >= MAX_ADDITIVE_PERM_DIRECTIONS) {
                            return false;
                        }
                        bounds->direction_first[row][direction] =
                            (uint16_t)first;
                        bounds->direction_second[row][direction] =
                            (uint16_t)second;
                        ++direction;
                    }
                }
            } else {
                unsigned second;

                for (second = 1; second < permutation; ++second) {
                    if (direction >= MAX_ADDITIVE_PERM_DIRECTIONS) {
                        return false;
                    }
                    bounds->direction_first[row][direction] = 0;
                    bounds->direction_second[row][direction] =
                        (uint16_t)second;
                    ++direction;
                }
            }
            bounds->direction_count[row] = direction;
        }
#endif
    }
    return true;
}

static uint64_t saturating_add(uint64_t first, uint64_t second)
{
    return UINT64_MAX - first < second ? UINT64_MAX : first + second;
}

static uint64_t additive_perm_contribution_at(
    const struct search *search, const struct additive_perm_bounds *bounds,
    unsigned row, unsigned candidate, unsigned actual_column,
    unsigned permutation)
{
    unsigned face = search->grid[candidate][actual_column];
    uint64_t contribution = bounds->contribution[row][face][permutation];

#if FULL_MIRROR
    {
        unsigned mirror_column = SIDES - actual_column - 1U;
        unsigned mirror_face = search->grid[candidate][mirror_column];

        contribution += bounds->contribution[row][mirror_face][permutation];
    }
#elif MIRROR_COLUMNS > 0
    if (column_has_mirror(actual_column)) {
        unsigned mirror_column = SIDES - actual_column - 1U;
        unsigned mirror_face = search->grid[candidate][mirror_column];

        contribution += bounds->contribution[row][mirror_face][permutation];
    }
#endif
    return contribution;
}

static void build_additive_perm_bounds(struct search *search, unsigned row)
{
    struct additive_perm_bounds *bounds = &search->additive_permutations;
    const struct perm_counter *counter = &search->permutations;
    unsigned permutation_count = bounds->permutation_count[row];
    unsigned face;
    unsigned permutation;
    unsigned column;

    build_owner_table(search);
    memset(bounds->reverse_ways, 0, sizeof(bounds->reverse_ways));
    bounds->reverse_ways[0] = 1;
    face = FACE_COUNT;
    while (face-- > 0) {
        for (permutation = 0; permutation < permutation_count;
             ++permutation) {
            bounds->above[face][permutation] = bounds->reverse_ways[
                bounds->reverse_suffix_state[row][permutation]];
        }
        if (search->owner[face] < row) {
            add_owner_to_perm_ways(
                counter, bounds->reverse_ways, search->owner[face]);
        }
    }

    memset(bounds->lower_ways, 0, sizeof(bounds->lower_ways));
    bounds->lower_ways[0] = 1;
    for (face = 0; face < FACE_COUNT; ++face) {
        for (permutation = 0; permutation < permutation_count;
             ++permutation) {
            uint64_t below = bounds->lower_ways[
                bounds->prefix_state[row][permutation]];
            uint64_t above = bounds->above[face][permutation];

            bounds->contribution[row][face][permutation] = below * above;
        }
        if (search->owner[face] < row) {
            add_owner_to_perm_ways(
                counter, bounds->lower_ways, search->owner[face]);
        }
    }

    memset(bounds->tally[row], 0, sizeof(bounds->tally[row]));
#if ROW2_MITM_ACTIVE
    if (row == DICE - 2U && !search->template_active) {
        return;
    }
#endif
    memset(bounds->minimum_left[row][SEARCH_COLUMNS], 0,
           sizeof(bounds->minimum_left[row][SEARCH_COLUMNS]));
    memset(bounds->maximum_left[row][SEARCH_COLUMNS], 0,
           sizeof(bounds->maximum_left[row][SEARCH_COLUMNS]));
#if ADDITIVE_PERM_LINEAR_ACTIVE
    memset(bounds->direction_minimum_left[row][SEARCH_COLUMNS], 0,
           bounds->direction_count[row] * sizeof(int64_t));
    memset(bounds->direction_maximum_left[row][SEARCH_COLUMNS], 0,
           bounds->direction_count[row] * sizeof(int64_t));
#endif
    column = SEARCH_COLUMNS;
    while (column-- > 0) {
        unsigned actual_column = ordered_column(search, row, column);
        unsigned candidate_mask = search->candidate_mask[row][column];

        for (permutation = 0; permutation < permutation_count;
             ++permutation) {
            uint64_t minimum = UINT64_MAX;
            uint64_t maximum = 0;
            unsigned candidate;

            for (candidate = row; candidate < DICE; ++candidate) {
                uint64_t contribution = additive_perm_contribution_at(
                    search, bounds, row, candidate, actual_column,
                    permutation);

                if ((candidate_mask & (1U << candidate)) == 0) {
                    continue;
                }
                if (contribution < minimum) {
                    minimum = contribution;
                }
                if (contribution > maximum) {
                    maximum = contribution;
                }
            }
            bounds->minimum_left[row][column][permutation] = saturating_add(
                minimum, bounds->minimum_left[row][column + 1U][permutation]);
            bounds->maximum_left[row][column][permutation] = saturating_add(
                maximum, bounds->maximum_left[row][column + 1U][permutation]);
        }
#if ADDITIVE_PERM_LINEAR_ACTIVE
        if (bounds->linear_possible[row]) {
            unsigned direction;

            for (direction = 0; direction < bounds->direction_count[row];
                 ++direction) {
                unsigned first = bounds->direction_first[row][direction];
                unsigned second = bounds->direction_second[row][direction];
                unsigned candidate =
                    (unsigned)__builtin_ctz(candidate_mask);
                int64_t minimum =
                    (int64_t)additive_perm_contribution_at(
                        search, bounds, row, candidate, actual_column,
                        first) -
                    (int64_t)additive_perm_contribution_at(
                        search, bounds, row, candidate, actual_column,
                        second);
                int64_t maximum = minimum;

                for (++candidate; candidate < DICE; ++candidate) {
                    int64_t difference =
                        (int64_t)additive_perm_contribution_at(
                            search, bounds, row, candidate, actual_column,
                            first) -
                        (int64_t)additive_perm_contribution_at(
                            search, bounds, row, candidate, actual_column,
                            second);

                    if ((candidate_mask & (1U << candidate)) == 0) {
                        continue;
                    }

                    if (difference < minimum) {
                        minimum = difference;
                    }
                    if (difference > maximum) {
                        maximum = difference;
                    }
                }
                bounds->direction_minimum_left[row][column][direction] =
                    minimum + bounds->direction_minimum_left
                        [row][column + 1U][direction];
                bounds->direction_maximum_left[row][column][direction] =
                    maximum + bounds->direction_maximum_left
                        [row][column + 1U][direction];
            }
        }
#endif
    }
}

static bool additive_perm_bounds_allow_goal(const struct search *search,
                                            unsigned row, unsigned column)
{
    const struct additive_perm_bounds *bounds =
        &search->additive_permutations;
    unsigned permutation;

    if (row < 2U) {
        return true;
    }
    for (permutation = 0;
         permutation < bounds->permutation_count[row]; ++permutation) {
        uint64_t current = bounds->tally[row][permutation];
        uint64_t goal = bounds->goal[row];

        if (current > goal ||
            bounds->minimum_left[row][column][permutation] > goal - current ||
            bounds->maximum_left[row][column][permutation] < goal - current) {
            return false;
        }
    }
    return true;
}

#if ADDITIVE_PERM_LINEAR_ACTIVE
static bool additive_perm_direction_bounds_allow_goal(
    const struct search *search, unsigned row, unsigned column)
{
    const struct additive_perm_bounds *bounds =
        &search->additive_permutations;
    unsigned direction;

    if (row < 2U || !bounds->linear_possible[row] ||
        column == SEARCH_COLUMNS) {
        return true;
    }
#if ADDITIVE_PERM_LINEAR_BOUND_STRIDE != 1
    if (column % ADDITIVE_PERM_LINEAR_BOUND_STRIDE != 0) {
        return true;
    }
#endif
    for (direction = 0; direction < bounds->direction_count[row];
         ++direction) {
        unsigned first = bounds->direction_first[row][direction];
        unsigned second = bounds->direction_second[row][direction];
        int64_t current = (int64_t)bounds->tally[row][first] -
                          (int64_t)bounds->tally[row][second];

        if (current +
                bounds->direction_minimum_left[row][column][direction] > 0 ||
            current +
                bounds->direction_maximum_left[row][column][direction] < 0) {
            return false;
        }
    }
    return true;
}
#endif

static void add_additive_perm_choice(struct search *search, unsigned row,
                                     unsigned column, bool add)
{
    struct additive_perm_bounds *bounds = &search->additive_permutations;
    unsigned actual_column;
    unsigned permutation;

    if (row < 2U) {
        return;
    }
    actual_column = ordered_column(search, row, column);
    for (permutation = 0;
         permutation < bounds->permutation_count[row]; ++permutation) {
        uint64_t contribution = additive_perm_contribution_at(
            search, bounds, row, row, actual_column, permutation);
        if (add) {
            bounds->tally[row][permutation] += contribution;
        } else {
            bounds->tally[row][permutation] -= contribution;
        }
    }
}

#endif

#if !FULL_MIRROR
static bool pair_tracking_active(unsigned column)
{
#if PAIR_BOUND_START == 0
    (void)column;
    return true;
#else
    return column >= PAIR_BOUND_START;
#endif
}

static unsigned pair_choice_win(const struct search *search,
                                unsigned candidate, unsigned previous,
                                unsigned actual_column)
{
#if MIRROR_COLUMNS > 0
    if (column_has_mirror(actual_column)) {
        return 0;
    }
#endif
    return search->grid[candidate][actual_column] >
        search->grid[previous][actual_column];
}
#endif

static uint64_t choice_contribution_at(const struct search *search,
                                       unsigned candidate,
                                       unsigned actual_column,
                                       unsigned place)
{
    unsigned face = search->grid[candidate][actual_column];
    uint64_t contribution = search->contribution[face][place];

#if FULL_MIRROR
    {
        unsigned mirror_column = SIDES - actual_column - 1U;
        unsigned mirror_face = search->grid[candidate][mirror_column];

        contribution += search->contribution[mirror_face][place];
    }
#elif MIRROR_COLUMNS > 0
    if (column_has_mirror(actual_column)) {
        unsigned mirror_column = SIDES - actual_column - 1U;
        unsigned mirror_face = search->grid[candidate][mirror_column];

        contribution += search->contribution[mirror_face][place];
    }
#endif
    return contribution;
}

static uint64_t choice_contribution(const struct search *search,
                                    unsigned row, unsigned candidate,
                                    unsigned column, unsigned place)
{
    return choice_contribution_at(
        search, candidate, ordered_column(search, row, column), place);
}

/*
 * Column zero stays fixed to remove equivalent die renamings.  The remaining
 * columns are stably sorted by the number of choices that can still satisfy
 * the place, linear-place, and pairwise bounds.  The smallest surviving domain
 * is tried first, while ties retain the useful HIGH_FIRST/LOW_FIRST bias.
 * This lookahead is performed only when the row configuration is entered;
 * recursive nodes do no ordering work.
 */
static void plan_column_order(struct search *search, unsigned row)
{
    uint64_t minimum[SEARCH_COLUMNS][DICE];
    uint64_t maximum[SEARCH_COLUMNS][DICE];
    uint64_t total_minimum[DICE] = {0};
    uint64_t total_maximum[DICE] = {0};
    int64_t direction_minimum[SEARCH_COLUMNS][PLACE_DIRECTION_COUNT];
    int64_t direction_maximum[SEARCH_COLUMNS][PLACE_DIRECTION_COUNT];
    int64_t direction_total_minimum[PLACE_DIRECTION_COUNT] = {0};
    int64_t direction_total_maximum[PLACE_DIRECTION_COUNT] = {0};
#if !FULL_MIRROR
    unsigned pair_minimum[SEARCH_COLUMNS][DICE];
    unsigned pair_maximum[SEARCH_COLUMNS][DICE];
    unsigned pair_total_minimum[DICE] = {0};
    unsigned pair_total_maximum[DICE] = {0};
#endif
    unsigned feasible_choices[SEARCH_COLUMNS];
    unsigned column;
    unsigned place;

    reset_column_order(search, row);
    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        unsigned actual_column = search->column_order[row][column];
        unsigned candidate_mask = available_candidate_mask(
            search, row, actual_column,
            !search->template_active && column == 0);

        for (place = 0; place < DICE; ++place) {
            uint64_t low = UINT64_MAX;
            uint64_t high = 0;
            unsigned candidate;

            for (candidate = row; candidate < DICE; ++candidate) {
                uint64_t value = choice_contribution_at(
                    search, candidate, actual_column, place);

                if ((candidate_mask & (1U << candidate)) == 0) {
                    continue;
                }

                if (value < low) {
                    low = value;
                }
                if (value > high) {
                    high = value;
                }
            }
            minimum[column][place] = low;
            maximum[column][place] = high;
            total_minimum[place] += low;
            total_maximum[place] += high;
        }
    }

    if (search->linear_place_bounds_possible) {
        unsigned first;
        unsigned direction = 0;

        for (first = 0; first < DICE; ++first) {
            unsigned second;

            for (second = first + 1U; second < DICE; ++second) {
                for (column = 0; column < SEARCH_COLUMNS; ++column) {
                    unsigned actual_column =
                        search->column_order[row][column];
                    unsigned candidate_mask = available_candidate_mask(
                        search, row, actual_column,
                        !search->template_active && column == 0);
                    unsigned candidate =
                        (unsigned)__builtin_ctz(candidate_mask);
                    int64_t low =
                        (int64_t)choice_contribution_at(
                            search, candidate, actual_column, first) -
                        (int64_t)choice_contribution_at(
                            search, candidate, actual_column, second);
                    int64_t high = low;

                    for (++candidate; candidate < DICE; ++candidate) {
                        int64_t value = (int64_t)choice_contribution_at(
                            search, candidate, actual_column, first) -
                            (int64_t)choice_contribution_at(
                                search, candidate, actual_column, second);

                        if ((candidate_mask & (1U << candidate)) == 0) {
                            continue;
                        }

                        if (value < low) {
                            low = value;
                        }
                        if (value > high) {
                            high = value;
                        }
                    }
                    direction_minimum[column][direction] = low;
                    direction_maximum[column][direction] = high;
                    direction_total_minimum[direction] += low;
                    direction_total_maximum[direction] += high;
                }
                ++direction;
            }
        }
    }

#if !FULL_MIRROR
    {
        unsigned previous;

        for (previous = 0; previous < row; ++previous) {
            for (column = 0; column < SEARCH_COLUMNS; ++column) {
                unsigned actual_column =
                    search->column_order[row][column];
                unsigned candidate_mask = available_candidate_mask(
                    search, row, actual_column,
                    !search->template_active && column == 0);
                unsigned low = 1;
                unsigned high = 0;
                unsigned candidate;

                for (candidate = row; candidate < DICE; ++candidate) {
                    unsigned value = pair_choice_win(
                        search, candidate, previous, actual_column);

                    if ((candidate_mask & (1U << candidate)) == 0) {
                        continue;
                    }

                    if (value < low) {
                        low = value;
                    }
                    if (value > high) {
                        high = value;
                    }
                }
                pair_minimum[column][previous] = low;
                pair_maximum[column][previous] = high;
                pair_total_minimum[previous] += low;
                pair_total_maximum[previous] += high;
            }
        }
    }
#endif

    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        unsigned actual_column = search->column_order[row][column];
        unsigned candidate_mask = available_candidate_mask(
            search, row, actual_column,
            !search->template_active && column == 0);
        unsigned feasible = 0;
        unsigned candidate;

        for (candidate = row; candidate < DICE; ++candidate) {
            bool allowed = true;

            if ((candidate_mask & (1U << candidate)) == 0) {
                continue;
            }

            for (place = 0; place < DICE; ++place) {
                uint64_t value = choice_contribution_at(
                    search, candidate, actual_column, place);
                uint64_t low = total_minimum[place] -
                    minimum[column][place];
                uint64_t high = total_maximum[place] -
                    maximum[column][place];

                if (value > search->place_goal ||
                    low > search->place_goal - value ||
                    high < search->place_goal - value) {
                    allowed = false;
                    break;
                }
            }
            if (allowed && search->linear_place_bounds_possible) {
                unsigned first;
                unsigned direction = 0;

                for (first = 0; first < DICE && allowed; ++first) {
                    unsigned second;

                    for (second = first + 1U; second < DICE; ++second) {
                        int64_t value =
                            (int64_t)choice_contribution_at(
                                search, candidate, actual_column, first) -
                            (int64_t)choice_contribution_at(
                                search, candidate, actual_column, second);
                        int64_t needed = -value;
                        int64_t low = direction_total_minimum[direction] -
                            direction_minimum[column][direction];
                        int64_t high = direction_total_maximum[direction] -
                            direction_maximum[column][direction];

                        if (low > needed || high < needed) {
                            allowed = false;
                            break;
                        }
                        ++direction;
                    }
                }
            }
#if !FULL_MIRROR
            if (allowed) {
                unsigned previous;

                for (previous = 0; previous < row; ++previous) {
                    unsigned value = pair_choice_win(
                        search, candidate, previous, actual_column);
                    unsigned low = pair_total_minimum[previous] -
                        pair_minimum[column][previous];
                    unsigned high = pair_total_maximum[previous] -
                        pair_maximum[column][previous];

                    if (value + low > PAIR_GOAL ||
                        value + high < PAIR_GOAL) {
                        allowed = false;
                        break;
                    }
                }
            }
#endif
            if (allowed) {
                ++feasible;
            }
        }
        feasible_choices[column] = feasible;
    }

    for (column = search->template_active ? 1U : 2U;
         column < SEARCH_COLUMNS; ++column) {
        unsigned selected_column = search->column_order[row][column];
        unsigned selected_choices = feasible_choices[column];
        unsigned position = column;

        while (position > (search->template_active ? 0U : 1U) &&
               feasible_choices[position - 1U] > selected_choices) {
            search->column_order[row][position] =
                search->column_order[row][position - 1U];
            feasible_choices[position] = feasible_choices[position - 1U];
            --position;
        }
        search->column_order[row][position] = selected_column;
        feasible_choices[position] = selected_choices;
    }
    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        unsigned actual_column = search->column_order[row][column];

        search->candidate_mask[row][column] = available_candidate_mask(
            search, row, actual_column,
            !search->template_active && column == 0);
    }
}

#if INCREMENTAL_C_COMPLETION_ACTIVE
static void prepare_incremental_c_columns(struct search *search,
                                          unsigned row)
{
    unsigned canonical_column = physical_column(0);
    unsigned search_column = 0;
    unsigned actual_column;

    search->column_order[row][search_column++] = canonical_column;
    for (actual_column = 0; actual_column < SEARCH_COLUMNS;
         ++actual_column) {
        if (actual_column != canonical_column) {
            search->column_order[row][search_column++] = actual_column;
        }
    }
    for (search_column = 0; search_column < SEARCH_COLUMNS;
         ++search_column) {
        actual_column = search->column_order[row][search_column];
        search->candidate_mask[row][search_column] =
            available_candidate_mask(
                search, row, actual_column, search_column == 0);
    }
}

static unsigned incremental_c_boundary(unsigned search_column)
{
    unsigned canonical_column = physical_column(0);

    return canonical_column == 0U ? search_column :
        (search_column == 0U ? 0U : search_column - 1U);
}
#endif

#if ROW2_MITM_ACTIVE
static void prepare_mitm_columns(struct search *search, unsigned row)
{
    unsigned column;

    reset_column_order(search, row);
    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        unsigned actual_column = ordered_column(search, row, column);

        search->candidate_mask[row][column] = available_candidate_mask(
            search, row, actual_column, column == 0);
    }
}
#endif

/* Build independent min/max suffix bounds for one die being filled. */
static void build_bounds(struct search *search, unsigned row)
{
    unsigned place;

    for (place = 0; place < DICE; ++place) {
        int column;
        search->minimum_left[row][SEARCH_COLUMNS][place] = 0;
        search->maximum_left[row][SEARCH_COLUMNS][place] = 0;

        for (column = SEARCH_COLUMNS - 1; column >= 0; --column) {
            uint64_t minimum = UINT64_MAX;
            uint64_t maximum = 0;
            unsigned candidate;
            unsigned candidate_mask =
                search->candidate_mask[row][(unsigned)column];

            for (candidate = row; candidate < DICE; ++candidate) {
                uint64_t value = choice_contribution(
                    search, row, candidate, (unsigned)column, place);

                if ((candidate_mask & (1U << candidate)) == 0) {
                    continue;
                }
                if (value < minimum) {
                    minimum = value;
                }
                if (value > maximum) {
                    maximum = value;
                }
            }
            search->minimum_left[row][column][place] = minimum +
                search->minimum_left[row][column + 1][place];
            search->maximum_left[row][column][place] = maximum +
                search->maximum_left[row][column + 1][place];
        }
    }

    if (search->linear_place_bounds_possible) {
        unsigned first;
        unsigned direction = 0;

        for (first = 0; first < DICE; ++first) {
            unsigned second;

            for (second = first + 1U; second < DICE; ++second) {
                int column;

                search->direction_minimum_left[row][SEARCH_COLUMNS]
                                              [direction] = 0;
                search->direction_maximum_left[row][SEARCH_COLUMNS]
                                              [direction] = 0;
                for (column = SEARCH_COLUMNS - 1; column >= 0; --column) {
                    unsigned candidate_mask =
                        search->candidate_mask[row][(unsigned)column];
                    unsigned candidate =
                        (unsigned)__builtin_ctz(candidate_mask);
                    int64_t minimum = (int64_t)choice_contribution(
                        search, row, candidate, (unsigned)column, first) -
                        (int64_t)choice_contribution(
                            search, row, candidate, (unsigned)column,
                            second);
                    int64_t maximum = minimum;

                    for (++candidate; candidate < DICE; ++candidate) {
                        int64_t value = (int64_t)choice_contribution(
                            search, row, candidate, (unsigned)column, first) -
                            (int64_t)choice_contribution(
                                search, row, candidate, (unsigned)column,
                                second);

                        if ((candidate_mask & (1U << candidate)) == 0) {
                            continue;
                        }

                        if (value < minimum) {
                            minimum = value;
                        }
                        if (value > maximum) {
                            maximum = value;
                        }
                    }
                    search->direction_minimum_left[row][column][direction] =
                        minimum + search->direction_minimum_left
                            [row][column + 1][direction];
                    search->direction_maximum_left[row][column][direction] =
                        maximum + search->direction_maximum_left
                            [row][column + 1][direction];
                }
                ++direction;
            }
        }
    }

#if PACKED_PAIR_WINS
    {
        uint64_t lane_ones = 0;
        unsigned previous;
        int column;

        for (previous = 0; previous < row; ++previous) {
            lane_ones |= UINT64_C(1) << (previous * 8U);
        }
        search->pair_minimum_left[row][SEARCH_COLUMNS] = 0;
        search->pair_maximum_left[row][SEARCH_COLUMNS] = 0;
        for (column = SEARCH_COLUMNS - 1; column >= 0; --column) {
            unsigned actual_column = ordered_column(
                search, row, (unsigned)column);
            unsigned candidate_mask =
                search->candidate_mask[row][(unsigned)column];
            uint64_t any_wins = 0;
            uint64_t all_win = lane_ones;
            unsigned candidate;

            for (candidate = row; candidate < DICE; ++candidate) {
                uint64_t increment = 0;

                if ((candidate_mask & (1U << candidate)) == 0) {
                    continue;
                }

                for (previous = 0; previous < row; ++previous) {
                    if (pair_choice_win(search, candidate, previous,
                                        actual_column)) {
                        increment |= UINT64_C(1) << (previous * 8U);
                    }
                }
                search->pair_increment[row][column][candidate] = increment;
                any_wins |= increment;
                all_win &= increment;
            }
            search->pair_minimum_left[row][column] = all_win +
                search->pair_minimum_left[row][column + 1];
            search->pair_maximum_left[row][column] = any_wins +
                search->pair_maximum_left[row][column + 1];
        }
    }
#endif

}


static bool bounds_allow_goal(const struct search *search,
                              unsigned row, unsigned column)
{
    unsigned place;

    for (place = 0; place < DICE; ++place) {
        uint64_t current = search->tally[row][place];
        uint64_t needed;

        if (current > search->place_goal) {
            return false;
        }
        needed = search->place_goal - current;
        if (search->minimum_left[row][column][place] > needed ||
            search->maximum_left[row][column][place] < needed) {
            return false;
        }
    }
    return true;
}

static bool direction_bounds_allow_goal(const struct search *search,
                                        unsigned row, unsigned column)
{
    unsigned first;
    unsigned direction = 0;

    if (!search->linear_place_bounds_possible) {
        return true;
    }
    /* Coordinate bounds already require exact tallies at the terminal node. */
    if (column == SEARCH_COLUMNS) {
        return true;
    }
#if LINEAR_BOUND_START != 0
    if (column < LINEAR_BOUND_START) {
        return true;
    }
#endif
#if LINEAR_BOUND_STRIDE != 1
    if (column % LINEAR_BOUND_STRIDE != 0) {
        return true;
    }
#endif
    for (first = 0; first < DICE; ++first) {
        unsigned second;

        for (second = first + 1U; second < DICE; ++second) {
            int64_t current = (int64_t)search->tally[row][first] -
                              (int64_t)search->tally[row][second];

            if (current +
                    search->direction_minimum_left[row][column][direction] >
                    0 ||
                current +
                    search->direction_maximum_left[row][column][direction] <
                    0) {
                return false;
            }
            ++direction;
        }
    }
    return true;
}

static bool pair_bounds_allow_goal(const struct search *search,
                                   unsigned row, unsigned column)
{
#if FULL_MIRROR
    (void)search;
    (void)row;
    (void)column;
    return true;
#else
    unsigned previous;
    unsigned goal = PAIR_GOAL;
    unsigned remaining;

    /* Large-side fallback builds may defer tracking to limit bookkeeping. */
    if (row == 0) {
        return true;
    }
#if PAIR_BOUND_START != 0
    if (column < PAIR_BOUND_START) {
        return true;
    }
#endif
    remaining = SEARCH_COLUMNS - column;
    for (previous = 0; previous < row; ++previous) {
#if PACKED_PAIR_WINS
        unsigned shift = previous * 8U;
        unsigned current = (unsigned)((search->pair_wins[row] >>
                                       shift) & UINT64_C(0xff));
        unsigned minimum = (unsigned)(
            (search->pair_minimum_left[row][column] >> shift) &
            UINT64_C(0xff));
        unsigned maximum = (unsigned)(
            (search->pair_maximum_left[row][column] >> shift) &
            UINT64_C(0xff));
        unsigned needed;
#else
        unsigned current = search->pair_wins[row][previous];
#endif

        if (current > goal || current + remaining < goal) {
            return false;
        }
#if PACKED_PAIR_WINS
        needed = goal - current;
        if (minimum > needed || maximum < needed) {
            return false;
        }
#endif
    }
    return true;
#endif
}

#if !FULL_MIRROR
static void initialize_pair_wins(struct search *search, unsigned row,
                                 unsigned assigned_columns)
{
    unsigned previous;
#if PACKED_PAIR_WINS
    uint64_t packed_wins = 0;
#endif

    for (previous = 0; previous < row; ++previous) {
        unsigned wins = 0;
        unsigned column;

        for (column = 0; column < assigned_columns; ++column) {
            unsigned actual_column = ordered_column(search, row, column);
            if (pair_choice_win(search, row, previous, actual_column)) {
                ++wins;
            }
        }
#if PACKED_PAIR_WINS
        packed_wins |= (uint64_t)wins << (previous * 8U);
#else
        search->pair_wins[row][previous] = wins;
#endif
    }
#if PACKED_PAIR_WINS
    search->pair_wins[row] = packed_wins;
#endif
}
#endif

static bool configuration_is_place_fair(const struct search *search)
{
    unsigned die;

    for (die = 0; die < DICE; ++die) {
        unsigned place;
        for (place = 0; place < DICE; ++place) {
            uint64_t tally = 0;
            unsigned column;
            for (column = 0; column < SIDES; ++column) {
                tally += search->contribution[search->grid[die][column]][place];
            }
            if (tally != search->place_goal) {
                return false;
            }
        }
    }
    return true;
}

/*
 * Every completed prefix is one of the subsets that must be place-fair.
 * Count its ranks directly by scanning labels from low to high.
 */
#if !PERM_ONLY
static bool completed_prefix_is_place_fair(const struct search *search,
                                           unsigned dice_built)
{
    uint64_t tally[DICE][DICE] = {{0}};
    unsigned faces_below[DICE] = {0};
    uint64_t subset_outcomes;
    uint64_t goal;
    unsigned face;
    unsigned die;

    if (dice_built < 3U) {
        return true;
    }
    if (!integer_power(SIDES, dice_built, &subset_outcomes) ||
        subset_outcomes % dice_built != 0) {
        return false;
    }
    goal = subset_outcomes / dice_built;

    for (face = 0; face < FACE_COUNT; ++face) {
        unsigned owner = search->owner[face];
        uint64_t polynomial[DICE + 1] = {0};
        unsigned degree = 0;
        unsigned opponent;

        if (owner >= dice_built) {
            continue;
        }
        polynomial[0] = 1;
        for (opponent = 0; opponent < dice_built; ++opponent) {
            uint64_t next[DICE + 1] = {0};
            uint64_t below;
            uint64_t above;
            unsigned place;

            if (opponent == owner) {
                continue;
            }
            below = faces_below[opponent];
            above = SIDES - below;
            for (place = 0; place <= degree; ++place) {
                next[place] += polynomial[place] * below;
                next[place + 1U] += polynomial[place] * above;
            }
            memcpy(polynomial, next, sizeof(polynomial));
            ++degree;
        }
        for (degree = 0; degree < dice_built; ++degree) {
            tally[owner][degree] += polynomial[degree];
        }
        ++faces_below[owner];
    }

    for (die = 0; die < dice_built; ++die) {
        unsigned place;

        if (faces_below[die] != SIDES) {
            return false;
        }
        for (place = 0; place < dice_built; ++place) {
            if (tally[die][place] != goal) {
                return false;
            }
        }
    }
    return true;
}
#endif

static double monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void publish_worker_stats(struct search *search)
{
    atomic_store_explicit(&search->published->nodes, search->nodes,
                          memory_order_relaxed);
#if ROW2_MITM_ACTIVE
    atomic_store_explicit(&search->published->mitm_solves,
                          search->mitm_solves, memory_order_relaxed);
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    atomic_store_explicit(&search->published->completion_checks,
                          search->completion_checks, memory_order_relaxed);
    atomic_store_explicit(&search->published->completion_prunes,
                          search->completion_prunes, memory_order_relaxed);
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    atomic_store_explicit(&search->published->early_completion_checks,
                          search->early_completion_checks,
                          memory_order_relaxed);
    atomic_store_explicit(&search->published->early_completion_prunes,
                          search->early_completion_prunes,
                          memory_order_relaxed);
    atomic_store_explicit(&search->published->early_completion_states,
                          search->early_completion_states,
                          memory_order_relaxed);
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
    atomic_store_explicit(&search->published->c_completion_checks,
                          search->c_completion_checks,
                          memory_order_relaxed);
    atomic_store_explicit(&search->published->c_completion_prunes,
                          search->c_completion_prunes,
                          memory_order_relaxed);
#if INCREMENTAL_C_COMPLETION_COUPLED
    atomic_store_explicit(
        &search->published->c_completion_coupled_prunes,
        search->c_completion_coupled_prunes, memory_order_relaxed);
#endif
    atomic_store_explicit(&search->published->c_completion_states,
                          search->c_completion_states,
                          memory_order_relaxed);
#endif
#endif
    atomic_store_explicit(&search->published->bound_prunes,
                          search->bound_prunes, memory_order_relaxed);
    atomic_store_explicit(&search->published->linear_place_prunes,
                          search->linear_place_prunes,
                          memory_order_relaxed);
    atomic_store_explicit(&search->published->pair_bound_prunes,
                          search->pair_bound_prunes,
                          memory_order_relaxed);
    atomic_store_explicit(&search->published->prefix_place_prunes,
                          search->prefix_place_prunes,
                          memory_order_relaxed);
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    atomic_store_explicit(&search->published->additive_perm_prunes,
                          search->additive_perm_prunes,
                          memory_order_relaxed);
#if ADDITIVE_PERM_LINEAR_ACTIVE
    atomic_store_explicit(&search->published->linear_perm_prunes,
                          search->linear_perm_prunes,
                          memory_order_relaxed);
#endif
#endif
    atomic_store_explicit(&search->published->all_subset_place_prunes,
                          search->all_subset_place_prunes,
                          memory_order_relaxed);
    atomic_store_explicit(
        &search->published->all_subset_place_fair_count,
        search->all_subset_place_fair_count, memory_order_relaxed);
    atomic_store_explicit(&search->published->permutation_fair_count,
                          search->permutation_fair_count,
                          memory_order_relaxed);
}

static const char *solution_kind_name(enum solution_kind kind)
{
    return kind == ALL_SUBSET_PLACE_FAIR
        ? "all-subset-place-fair"
        : "permutation-fair";
}

static void record_solution(struct search *search, enum solution_kind kind)
{
    struct shared_state *shared = search->shared;
    atomic_uint_fast64_t *total = kind == ALL_SUBSET_PLACE_FAIR
        ? &shared->all_subset_total
        : &shared->permutation_total;
    uint64_t number;
    unsigned canonical_die[DICE];
    unsigned face;
    bool flush_ready = false;
    bool retain_for_mirror_count = !FULL_MIRROR &&
        kind == PERMUTATION_FAIR;

    /* Suppressed results need only the final atomic accounting update. */
    if (shared->options.solutions_path == NULL &&
        !retain_for_mirror_count &&
        atomic_load_explicit(total, memory_order_relaxed) >=
            shared->options.print_limit) {
        atomic_fetch_add_explicit(total, 1, memory_order_relaxed);
        return;
    }

    pthread_mutex_lock(&shared->solution_mutex);
    while (shared->solution_count == SOLUTION_QUEUE_CAPACITY) {
        pthread_cond_wait(&shared->solution_not_full,
                          &shared->solution_mutex);
    }
    number = atomic_fetch_add_explicit(total, 1,
                                       memory_order_relaxed) + 1U;
    if (number > shared->options.print_limit &&
        shared->options.solutions_path == NULL &&
        !retain_for_mirror_count) {
        pthread_mutex_unlock(&shared->solution_mutex);
        return;
    }

    /*
     * Search-order symmetry breaking may fix a physical column other than
     * column zero.  Relabel only the reported result so its first physical
     * column is always ABC..., independent of traversal order and templates.
     * owner[0..DICE-1] is a permutation, so it directly defines the map from
     * internal search rows to canonical output labels.
     */
    for (face = 0; face < DICE; ++face) {
        canonical_die[search->owner[face]] = face;
    }
    {
        struct solution *solution =
            &shared->solution_queue[shared->solution_count];

        solution->kind = kind;
        solution->number = number;
        for (face = 0; face < FACE_COUNT; ++face) {
            solution->encoding[face] =
                (char)('A' + canonical_die[search->owner[face]]);
        }
        solution->encoding[FACE_COUNT] = '\0';
    }
    ++shared->solution_count;
    flush_ready = shared->solution_count == SOLUTION_FLUSH_THRESHOLD;
    pthread_mutex_unlock(&shared->solution_mutex);

    /*
     * Once a useful file-write batch is ready, synchronize with the watcher's
     * condition wait so the notification cannot be lost between its queue
     * check and pthread_cond_timedwait().  Partial batches are flushed by the
     * one-second watcher tick and by shutdown.
     */
    if (flush_ready) {
        pthread_mutex_lock(&shared->completion_mutex);
        pthread_cond_signal(&shared->completion_condition);
        pthread_mutex_unlock(&shared->completion_mutex);
    }
}

static void accept_configuration(struct search *search)
{
    struct shared_state *shared = search->shared;
    uint64_t limit_claim = 0;
    bool permutation_fair;

    /* The last die is implied; verify the invariant before reporting it. */
    if (!configuration_is_place_fair(search)) {
        atomic_store_explicit(&shared->internal_error, true,
                              memory_order_relaxed);
        atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
        return;
    }

    build_owner_table(search);
    count_permutation_ways(search, (1U << DICE) - 1U);
#if PERM_ONLY
    permutation_fair = search->permutation_fairness_possible &&
        counted_subset_is_permutation_fair(
            search, (1U << DICE) - 1U, DICE);
    if (!permutation_fair) {
        return;
    }
#else
    if (!counted_all_subsets_are_place_fair(search)) {
        ++search->all_subset_place_prunes;
        return;
    }

    permutation_fair = search->permutation_fairness_possible &&
        counted_subset_is_permutation_fair(
            search, (1U << DICE) - 1U, DICE);
#endif

    if (shared->options.limit != 0) {
        limit_claim = atomic_fetch_add_explicit(&shared->limit_claims, 1,
                                                memory_order_relaxed);
        if (limit_claim >= shared->options.limit) {
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
            return;
        }
    }

#if PERM_ONLY
    ++search->permutation_fair_count;
    record_solution(search, PERMUTATION_FAIR);
#else
    if (permutation_fair) {
        ++search->permutation_fair_count;
        record_solution(search, PERMUTATION_FAIR);
    } else {
        ++search->all_subset_place_fair_count;
        record_solution(search, ALL_SUBSET_PLACE_FAIR);
    }
#endif

    if (shared->options.limit != 0 &&
        limit_claim + 1U >= shared->options.limit) {
        atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
    }
}

static void apply_choice(struct search *search, unsigned row,
                         unsigned column, unsigned candidate)
{
    unsigned actual_column = ordered_column(search, row, column);
    unsigned temporary = search->grid[row][actual_column];
    unsigned chosen_face;
    unsigned place;

    search->grid[row][actual_column] = search->grid[candidate][actual_column];
    search->grid[candidate][actual_column] = temporary;
    chosen_face = search->grid[row][actual_column];

#if !FULL_MIRROR
    if (pair_tracking_active(column)) {
#if PACKED_PAIR_WINS
        search->pair_wins[row] +=
            search->pair_increment[row][column][candidate];
#else
        unsigned previous;

        for (previous = 0; previous < row; ++previous) {
            if (pair_choice_win(search, row, previous, actual_column)) {
                ++search->pair_wins[row][previous];
            }
        }
#endif
    }
#endif

#if FULL_MIRROR
    {
        unsigned mirror_column = SIDES - actual_column - 1U;
        temporary = search->grid[row][mirror_column];
        search->grid[row][mirror_column] =
            search->grid[candidate][mirror_column];
        search->grid[candidate][mirror_column] = temporary;
    }
#elif MIRROR_COLUMNS > 0
    if (column_has_mirror(actual_column)) {
        unsigned mirror_column = SIDES - actual_column - 1U;

        temporary = search->grid[row][mirror_column];
        search->grid[row][mirror_column] =
            search->grid[candidate][mirror_column];
        search->grid[candidate][mirror_column] = temporary;
    }
#endif

    for (place = 0; place < DICE; ++place) {
        search->tally[row][place] += search->contribution[chosen_face][place];
#if FULL_MIRROR
        search->tally[row][place] += search->contribution[
            search->grid[row][SIDES - actual_column - 1U]][place];
#elif MIRROR_COLUMNS > 0
        if (column_has_mirror(actual_column)) {
            search->tally[row][place] += search->contribution[
                search->grid[row][SIDES - actual_column - 1U]][place];
        }
#endif
    }
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    add_additive_perm_choice(search, row, column, true);
#endif
}

static void undo_choice(struct search *search, unsigned row,
                        unsigned column, unsigned candidate)
{
    unsigned actual_column = ordered_column(search, row, column);
    unsigned chosen_face = search->grid[row][actual_column];
    unsigned place;
    unsigned temporary;

#if !FULL_MIRROR
    if (pair_tracking_active(column)) {
#if PACKED_PAIR_WINS
        search->pair_wins[row] -=
            search->pair_increment[row][column][candidate];
#else
        unsigned previous;

        for (previous = 0; previous < row; ++previous) {
            if (pair_choice_win(search, row, previous, actual_column)) {
                --search->pair_wins[row][previous];
            }
        }
#endif
    }
#endif

    for (place = 0; place < DICE; ++place) {
        search->tally[row][place] -= search->contribution[chosen_face][place];
#if FULL_MIRROR
        search->tally[row][place] -= search->contribution[
            search->grid[row][SIDES - actual_column - 1U]][place];
#elif MIRROR_COLUMNS > 0
        if (column_has_mirror(actual_column)) {
            search->tally[row][place] -= search->contribution[
                search->grid[row][SIDES - actual_column - 1U]][place];
        }
#endif
    }
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    add_additive_perm_choice(search, row, column, false);
#endif

    temporary = search->grid[row][actual_column];
    search->grid[row][actual_column] = search->grid[candidate][actual_column];
    search->grid[candidate][actual_column] = temporary;
#if FULL_MIRROR
    {
        unsigned mirror_column = SIDES - actual_column - 1U;
        temporary = search->grid[row][mirror_column];
        search->grid[row][mirror_column] =
            search->grid[candidate][mirror_column];
        search->grid[candidate][mirror_column] = temporary;
    }
#elif MIRROR_COLUMNS > 0
    if (column_has_mirror(actual_column)) {
        unsigned mirror_column = SIDES - actual_column - 1U;

        temporary = search->grid[row][mirror_column];
        search->grid[row][mirror_column] =
            search->grid[candidate][mirror_column];
        search->grid[candidate][mirror_column] = temporary;
    }
#endif
}

static bool completed_row_is_fair(struct search *search, unsigned row)
{
    unsigned place;

    for (place = 0; place < DICE; ++place) {
        if (search->tally[row][place] != search->place_goal) {
            return false;
        }
    }
#if !PERM_ONLY
    if (row + 1U >= 3U) {
        build_owner_table(search);
    }
    if (!completed_prefix_is_place_fair(search, row + 1U)) {
        return false;
    }
#endif
    return true;
}

#if ROW2_MITM_ACTIVE
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
static bool next_column_permutation(uint8_t values[DICE])
{
    unsigned pivot = DICE - 1U;
    unsigned successor;
    unsigned left;
    unsigned right;

    while (pivot != 0U && values[pivot - 1U] >= values[pivot]) {
        --pivot;
    }
    if (pivot == 0U) {
        return false;
    }
    successor = DICE - 1U;
    while (values[successor] <= values[pivot - 1U]) {
        --successor;
    }
    {
        uint8_t temporary = values[pivot - 1U];

        values[pivot - 1U] = values[successor];
        values[successor] = temporary;
    }
    left = pivot;
    right = DICE - 1U;
    while (left < right) {
        uint8_t temporary = values[left];

        values[left] = values[right];
        values[right] = temporary;
        ++left;
        --right;
    }
    return true;
}

static bool completion_block_matches(
    const struct conditioned_completion_bounds *bounds,
    unsigned permutation, const uint8_t word[DICE],
    unsigned begin, unsigned length)
{
    unsigned index;

    for (index = begin + 1U; index < begin + length; ++index) {
        if (bounds->positions[permutation][word[index - 1U]] >=
            bounds->positions[permutation][word[index]]) {
            return false;
        }
    }
    return true;
}

static unsigned choose_two(unsigned value)
{
    return value < 2U ? 0U : value * (value - 1U) / 2U;
}

#if DICE == 5
static unsigned choose_three(unsigned value)
{
    return value < 3U ? 0U :
        value * (value - 1U) * (value - 2U) / 6U;
}
#endif

static int32_t conditioned_unary_value(
    const struct conditioned_completion_bounds *bounds,
    unsigned permutation, const uint8_t word[DICE], unsigned column)
{
    unsigned before = column;
    unsigned after = SIDES - column - 1U;
    int32_t value = 0;

#if DICE == 4
    if (completion_block_matches(bounds, permutation, word, 0, 2)) {
        value += (int32_t)choose_two(after);
    }
    if (completion_block_matches(bounds, permutation, word, 0, 3)) {
        value += (int32_t)after;
    }
    if (completion_block_matches(bounds, permutation, word, 0, 4)) {
        ++value;
    }
    if (completion_block_matches(bounds, permutation, word, 1, 2)) {
        value += (int32_t)(before * after);
    }
    if (completion_block_matches(bounds, permutation, word, 1, 3)) {
        value += (int32_t)before;
    }
    if (completion_block_matches(bounds, permutation, word, 2, 2)) {
        value += (int32_t)choose_two(before);
    }
#else
    if (completion_block_matches(bounds, permutation, word, 0, 2)) {
        value += (int32_t)choose_three(after);
    }
    if (completion_block_matches(bounds, permutation, word, 1, 2)) {
        value += (int32_t)(before * choose_two(after));
    }
    if (completion_block_matches(bounds, permutation, word, 2, 2)) {
        value += (int32_t)(choose_two(before) * after);
    }
    if (completion_block_matches(bounds, permutation, word, 3, 2)) {
        value += (int32_t)choose_three(before);
    }
    if (completion_block_matches(bounds, permutation, word, 0, 3)) {
        value += (int32_t)choose_two(after);
    }
    if (completion_block_matches(bounds, permutation, word, 1, 3)) {
        value += (int32_t)(before * after);
    }
    if (completion_block_matches(bounds, permutation, word, 2, 3)) {
        value += (int32_t)choose_two(before);
    }
    if (completion_block_matches(bounds, permutation, word, 0, 4)) {
        value += (int32_t)after;
    }
    if (completion_block_matches(bounds, permutation, word, 1, 4)) {
        value += (int32_t)before;
    }
    if (completion_block_matches(bounds, permutation, word, 0, 5)) {
        ++value;
    }
#endif
    return value;
}

static bool initialize_conditioned_completion_bounds(struct search *search)
{
    struct conditioned_completion_bounds *bounds =
        &search->completion_bounds;
    uint8_t permutation_word[DICE];
    unsigned permutation = 0;
    unsigned word_count = 0;
    unsigned owner;

    for (owner = 0; owner < DICE; ++owner) {
        permutation_word[owner] = (uint8_t)owner;
    }
    do {
        unsigned position;
        bool fixed_first = true;
        bool variable_first = true;

        for (position = 0; position < DICE; ++position) {
            owner = permutation_word[position];
            bounds->positions[permutation][owner] = (uint8_t)position;
            if (position < DICE - 2U) {
                fixed_first &= owner < DICE - 2U;
            } else {
                fixed_first &= owner >= DICE - 2U;
            }
            if (position < 2U) {
                variable_first &= owner >= DICE - 2U;
            } else {
                variable_first &= owner < DICE - 2U;
            }
        }
        if (fixed_first || variable_first) {
            if (word_count > CONDITIONED_DIRECTION_COUNT) {
                return false;
            }
            memcpy(bounds->word[word_count], permutation_word,
                   sizeof(permutation_word));
            ++word_count;
        }
        ++permutation;
    } while (next_column_permutation(permutation_word));

    if (permutation != COLUMN_PERMUTATION_COUNT ||
        word_count != CONDITIONED_DIRECTION_COUNT + 1U) {
        return false;
    }
#if DICE == 4
    for (owner = 0; owner < CONDITIONED_DIRECTION_COUNT; ++owner) {
        unsigned column;
        unsigned index;

        for (column = 0; column < SIDES; ++column) {
            for (index = 0; index < COLUMN_PERMUTATION_COUNT; ++index) {
                int32_t difference = conditioned_unary_value(
                    bounds, index, bounds->word[owner + 1U], column) -
                    conditioned_unary_value(
                        bounds, index, bounds->word[0], column);

                if (difference < INT16_MIN || difference > INT16_MAX) {
                    return false;
                }
                bounds->unary[owner][column][index] = (int16_t)difference;
            }
        }
    }
#endif
    return true;
}

static unsigned column_permutation_rank(const uint8_t owners[DICE])
{
    static const unsigned factorial[DICE] = {
#if DICE == 4
        6U, 2U, 1U, 1U,
#else
        24U, 6U, 2U, 1U, 1U,
#endif
    };
    unsigned rank = 0;
    unsigned position;

    for (position = 0; position < DICE; ++position) {
        unsigned later;
        unsigned smaller = 0;

        for (later = position + 1U; later < DICE; ++later) {
            smaller += owners[later] < owners[position];
        }
        rank += smaller * factorial[position];
    }
    return rank;
}

static unsigned completion_actual_column_permutation_index(
    const struct search *search, unsigned row, unsigned actual_column,
    unsigned candidate)
{
    uint8_t owner_by_offset[DICE];
    unsigned die;

    for (die = 0; die < row; ++die) {
        owner_by_offset[search->grid[die][actual_column] % DICE] =
            (uint8_t)die;
    }
    owner_by_offset[search->grid[candidate][actual_column] % DICE] =
        (uint8_t)row;
    for (die = row; die < DICE; ++die) {
        if (die != candidate) {
            owner_by_offset[search->grid[die][actual_column] % DICE] =
                (uint8_t)(row + 1U);
        }
    }
    return column_permutation_rank(owner_by_offset);
}

static void conditioned_correlation_values(
    const struct conditioned_completion_bounds *bounds,
    const uint8_t word[DICE], const uint8_t base_index[SIDES],
    const uint8_t alternate_index[SIDES], int32_t base_value[SIDES],
    int32_t alternate_value[SIDES])
{
    bool fixed_first = word[0] < DICE - 2U;
    unsigned column;

    memset(base_value, 0, SIDES * sizeof(*base_value));
    memset(alternate_value, 0, SIDES * sizeof(*alternate_value));
#if DICE == 4
    if (fixed_first) {
        int prefix = 0;

        for (column = 0; column < SIDES; ++column) {
            base_value[column] = prefix * completion_block_matches(
                bounds, base_index[column], word, 2, 2);
            alternate_value[column] = prefix * completion_block_matches(
                bounds, alternate_index[column], word, 2, 2);
            prefix += completion_block_matches(
                bounds, base_index[column], word, 0, 2);
        }
    } else {
        int suffix = 0;

        column = SIDES;
        while (column-- > 0) {
            base_value[column] = suffix * completion_block_matches(
                bounds, base_index[column], word, 0, 2);
            alternate_value[column] = suffix * completion_block_matches(
                bounds, alternate_index[column], word, 0, 2);
            suffix += completion_block_matches(
                bounds, base_index[column], word, 2, 2);
        }
    }
#else
    if (fixed_first) {
        int prefix_01_count = 0;
        int prefix_01_columns = 0;
        int prefix_12_columns = 0;
        int prefix_012_count = 0;

        for (column = 0; column < SIDES; ++column) {
            unsigned index;

            for (index = 0; index < 2U; ++index) {
                unsigned permutation = index == 0U ? base_index[column] :
                    alternate_index[column];
                int32_t value = 0;

                value += prefix_01_count * (int)(SIDES - column - 1U) *
                    completion_block_matches(
                        bounds, permutation, word, 2, 2);
                value += (((int)column - 1) * prefix_01_count -
                          prefix_01_columns) * completion_block_matches(
                              bounds, permutation, word, 3, 2);
                value += prefix_12_columns * completion_block_matches(
                    bounds, permutation, word, 3, 2);
                value += prefix_01_count * completion_block_matches(
                    bounds, permutation, word, 2, 3);
                value += prefix_012_count * completion_block_matches(
                    bounds, permutation, word, 3, 2);
                if (index == 0U) {
                    base_value[column] = value;
                } else {
                    alternate_value[column] = value;
                }
            }
            if (completion_block_matches(
                    bounds, base_index[column], word, 0, 2)) {
                ++prefix_01_count;
                prefix_01_columns += (int)column;
            }
            if (completion_block_matches(
                    bounds, base_index[column], word, 1, 2)) {
                prefix_12_columns += (int)column;
            }
            prefix_012_count += completion_block_matches(
                bounds, base_index[column], word, 0, 3);
        }
    } else {
        int suffix_23_after = 0;
        int suffix_34_count = 0;
        int suffix_34_columns_minus_one = 0;
        int suffix_234_count = 0;

        column = SIDES;
        while (column-- > 0) {
            unsigned index;

            for (index = 0; index < 2U; ++index) {
                unsigned permutation = index == 0U ? base_index[column] :
                    alternate_index[column];
                int32_t value = 0;

                value += suffix_23_after * completion_block_matches(
                    bounds, permutation, word, 0, 2);
                value += (suffix_34_columns_minus_one -
                          (int)column * suffix_34_count) *
                    completion_block_matches(
                        bounds, permutation, word, 0, 2);
                value += (int)column * suffix_34_count *
                    completion_block_matches(
                        bounds, permutation, word, 1, 2);
                value += suffix_234_count * completion_block_matches(
                    bounds, permutation, word, 0, 2);
                value += suffix_34_count * completion_block_matches(
                    bounds, permutation, word, 0, 3);
                if (index == 0U) {
                    base_value[column] = value;
                } else {
                    alternate_value[column] = value;
                }
            }
            if (completion_block_matches(
                    bounds, base_index[column], word, 2, 2)) {
                suffix_23_after += (int)(SIDES - column - 1U);
            }
            if (completion_block_matches(
                    bounds, base_index[column], word, 3, 2)) {
                ++suffix_34_count;
                suffix_34_columns_minus_one += (int)column - 1;
            }
            suffix_234_count += completion_block_matches(
                bounds, base_index[column], word, 2, 3);
        }
    }
#endif
}

#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE || \
    INCREMENTAL_C_COMPLETION_ACTIVE
static const uint8_t early_completion_suffix_order
    [EARLY_COMPLETION_DIRECTION_COUNT][3] = {
        {2, 3, 4}, {2, 4, 3}, {3, 2, 4},
        {3, 4, 2}, {4, 2, 3}, {4, 3, 2},
    };
#endif

#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE

/*
 * Before C is built, BACDE-ABCDE has only one ordered nonlinear state.
 * Scanning physical columns from low to high gives
 *
 *     total += local(column choice) + G * I(DE)
 *     G     += I(BAC)-I(ABC)+column*(I(AC)-I(BC)).
 *
 * Keep a safe min/max total for each reachable G.  If no final interval
 * contains zero, no assignment of the remaining C/D/E faces can be fair.
 */
static bool early_conditioned_direction_possible(
    struct search *search, const uint8_t suffix[3])
{
    uint8_t reference[DICE] = {0, 1, suffix[0], suffix[1], suffix[2]};
    uint8_t target[DICE] = {1, 0, suffix[0], suffix[1], suffix[2]};
    const struct conditioned_completion_bounds *bounds =
        &search->completion_bounds;
    int64_t minimum[EARLY_COMPLETION_STATE_COUNT];
    int64_t maximum[EARLY_COMPLETION_STATE_COUNT];
    int64_t next_minimum[EARLY_COMPLETION_STATE_COUNT];
    int64_t next_maximum[EARLY_COMPLETION_STATE_COUNT];
    int fixed_prefix_count = 0;
    int fixed_prefix_columns = 0;
    unsigned column;
    unsigned state;

    for (state = 0; state < EARLY_COMPLETION_STATE_COUNT; ++state) {
        minimum[state] = INT64_MAX;
        maximum[state] = INT64_MIN;
    }
    minimum[EARLY_COMPLETION_G_LIMIT] = 0;
    maximum[EARLY_COMPLETION_G_LIMIT] = 0;

    for (column = 0; column < SIDES; ++column) {
        unsigned option[COLUMN_PERMUTATION_COUNT];
        unsigned option_count = 0;
        unsigned permutation;
        int fixed_gap = ((int)column - 1) * fixed_prefix_count -
            fixed_prefix_columns;

        for (permutation = 0; permutation < COLUMN_PERMUTATION_COUNT;
             ++permutation) {
            if (bounds->positions[permutation][0] ==
                    search->grid[0][column] % DICE &&
                bounds->positions[permutation][1] ==
                    search->grid[1][column] % DICE) {
                option[option_count++] = permutation;
            }
        }
        if (option_count != 6U) {
            return true;
        }
        for (state = 0; state < EARLY_COMPLETION_STATE_COUNT; ++state) {
            next_minimum[state] = INT64_MAX;
            next_maximum[state] = INT64_MIN;
        }
        for (state = 0; state < EARLY_COMPLETION_STATE_COUNT; ++state) {
            int old_g;
            unsigned choice;

            if (minimum[state] == INT64_MAX) {
                continue;
            }
            ++search->early_completion_states;
            old_g = (int)state - (int)EARLY_COMPLETION_G_LIMIT;
            for (choice = 0; choice < option_count; ++choice) {
                unsigned index = option[choice];
                int g = completion_block_matches(
                            bounds, index, target, 0, 3) -
                        completion_block_matches(
                            bounds, index, reference, 0, 3) +
                    (int)column *
                        (completion_block_matches(
                             bounds, index, target, 1, 2) -
                         completion_block_matches(
                             bounds, index, reference, 1, 2));
                int y = completion_block_matches(
                    bounds, index, reference, 3, 2);
                int64_t local = conditioned_unary_value(
                        bounds, index, target, column) -
                    conditioned_unary_value(
                        bounds, index, reference, column) +
                    (int64_t)fixed_prefix_count *
                        (int)(SIDES - column - 1U) *
                        completion_block_matches(
                            bounds, index, reference, 2, 2) +
                    (int64_t)fixed_gap * y +
                    (int64_t)fixed_prefix_count *
                        completion_block_matches(
                            bounds, index, reference, 2, 3);
                int new_g = old_g + g;
                unsigned next_state;
                int64_t candidate_minimum;
                int64_t candidate_maximum;

                if (new_g < -(int)EARLY_COMPLETION_G_LIMIT ||
                    new_g > (int)EARLY_COMPLETION_G_LIMIT) {
                    return true;
                }
                next_state = (unsigned)(new_g +
                    (int)EARLY_COMPLETION_G_LIMIT);
                candidate_minimum = minimum[state] + local +
                    (int64_t)old_g * y;
                candidate_maximum = maximum[state] + local +
                    (int64_t)old_g * y;
                if (candidate_minimum < next_minimum[next_state]) {
                    next_minimum[next_state] = candidate_minimum;
                }
                if (candidate_maximum > next_maximum[next_state]) {
                    next_maximum[next_state] = candidate_maximum;
                }
            }
        }
        memcpy(minimum, next_minimum, sizeof(minimum));
        memcpy(maximum, next_maximum, sizeof(maximum));
        fixed_prefix_count +=
            search->grid[1][column] < search->grid[0][column] ? 1 : -1;
        fixed_prefix_columns += (int)column *
            (search->grid[1][column] < search->grid[0][column] ? 1 : -1);
    }
    for (state = 0; state < EARLY_COMPLETION_STATE_COUNT; ++state) {
        if (minimum[state] <= 0 && maximum[state] >= 0) {
            return true;
        }
    }
    return false;
}

static bool early_conditioned_completion_possible(struct search *search)
{
    ++search->early_completion_checks;
    /* Relabeling unresolved C/D/E maps all six suffix orders bijectively. */
    if (!early_conditioned_direction_possible(
            search, early_completion_suffix_order[0])) {
        ++search->early_completion_prunes;
        return false;
    }
    return true;
}
#endif

#if INCREMENTAL_C_COMPLETION_ACTIVE
static bool c_completion_static_positions(const struct search *search,
                                          unsigned column,
                                          uint8_t *c_position,
                                          uint8_t *d_position)
{
    unsigned canonical_column = physical_column(0);

    /*
     * The canonical C/D cells (and their forced partners) are already fixed
     * when the table is built.  Treating every other mirror partner as free
     * is a deliberate relaxation until its low-column C choice is made.
     */
    if (column == canonical_column) {
        *c_position = (uint8_t)(search->grid[2][column] % DICE);
        *d_position = (uint8_t)(search->grid[3][column] % DICE);
        return true;
    }
#if MIRROR_COLUMNS > 0
    if (column_has_mirror(canonical_column) &&
        column == SIDES - canonical_column - 1U) {
        *c_position = (uint8_t)(search->grid[2][column] % DICE);
        *d_position = (uint8_t)(search->grid[3][column] % DICE);
        return true;
    }
#endif
    return false;
}

static unsigned c_completion_track_for_base_direction(unsigned direction)
{
    return direction < C_COMPLETION_BASE_SCALAR_DIRECTION_COUNT
        ? direction
        : direction + C_COMPLETION_COUPLED_DIRECTION_COUNT;
}

#if INCREMENTAL_C_COMPLETION_COUPLED
static bool build_coupled_c_transitions(struct search *search)
{
    struct incremental_c_completion *completion = &search->c_completion;
    unsigned column;

    for (column = 0; column < SIDES; ++column) {
        unsigned c_position;

        for (c_position = 0; c_position < DICE; ++c_position) {
            unsigned option;

            if (c_position == search->grid[0][column] % DICE ||
                c_position == search->grid[1][column] % DICE) {
                continue;
            }
            for (option = 0; option < 2U; ++option) {
                const struct incremental_c_transition *first =
                    &completion->transition[0][column][c_position][option];
                const struct incremental_c_transition *second =
                    &completion->transition[1][column][c_position][option];
                struct incremental_c_transition *plus =
                    &completion->transition[C_COMPLETION_PLUS_DIRECTION]
                                           [column][c_position][option];
                struct incremental_c_transition *minus =
                    &completion->transition[C_COMPLETION_MINUS_DIRECTION]
                                           [column][c_position][option];
                int64_t plus_local =
                    (int64_t)first->local + second->local;
                int64_t minus_local =
                    (int64_t)first->local - second->local;

                /*
                 * CDE and CED differ only in the final D/E order.  Their
                 * nonlinear G transition and physical D choice are shared,
                 * so support bounds for X+Y and X-Y remain one-dimensional.
                 */
                if (first->g != second->g ||
                    first->d_position != second->d_position ||
                    plus_local < INT32_MIN || plus_local > INT32_MAX ||
                    minus_local < INT32_MIN || minus_local > INT32_MAX) {
                    return false;
                }
                *plus = (struct incremental_c_transition){
                    .local = (int32_t)plus_local,
                    .g = first->g,
                    .y = (int8_t)(first->y + second->y),
                    .d_position = first->d_position,
                };
                *minus = (struct incremental_c_transition){
                    .local = (int32_t)minus_local,
                    .g = first->g,
                    .y = (int8_t)(first->y - second->y),
                    .d_position = first->d_position,
                };
            }
        }
    }
    return true;
}
#endif

static bool build_incremental_c_transitions(struct search *search)
{
    struct incremental_c_completion *completion = &search->c_completion;
    const struct conditioned_completion_bounds *bounds =
        &search->completion_bounds;
    int fixed_prefix_count[SIDES];
    int fixed_gap[SIDES];
    int count = 0;
    int column_sum = 0;
    unsigned column;
    unsigned direction;

    for (column = 0; column < SIDES; ++column) {
        int sign = search->grid[1][column] < search->grid[0][column]
            ? 1 : -1;

        fixed_prefix_count[column] = count;
        fixed_gap[column] = ((int)column - 1) * count - column_sum;
        count += sign;
        column_sum += (int)column * sign;
    }

    for (direction = 0;
         direction < C_COMPLETION_ACTIVE_BASE_DIRECTION_COUNT; ++direction) {
        unsigned track = c_completion_track_for_base_direction(direction);
        const uint8_t *suffix = early_completion_suffix_order[direction];
        uint8_t reference[DICE] = {
            0, 1, suffix[0], suffix[1], suffix[2]
        };
        uint8_t target[DICE] = {
            1, 0, suffix[0], suffix[1], suffix[2]
        };

        for (column = 0; column < SIDES; ++column) {
            uint8_t option_count[DICE] = {0};
            unsigned permutation;

            for (permutation = 0;
                 permutation < COLUMN_PERMUTATION_COUNT; ++permutation) {
                struct incremental_c_transition *transition;
                int g;
                int y;
                int64_t local;

                if (bounds->positions[permutation][0] !=
                        search->grid[0][column] % DICE ||
                    bounds->positions[permutation][1] !=
                        search->grid[1][column] % DICE) {
                    continue;
                }
                unsigned c_position = bounds->positions[permutation][2];
                unsigned option = option_count[c_position]++;

                if (option >= 2U) {
                    return false;
                }
                transition = &completion->transition[track][column]
                                                 [c_position][option];
                g = completion_block_matches(
                        bounds, permutation, target, 0, 3) -
                    completion_block_matches(
                        bounds, permutation, reference, 0, 3) +
                    (int)column *
                        (completion_block_matches(
                             bounds, permutation, target, 1, 2) -
                         completion_block_matches(
                             bounds, permutation, reference, 1, 2));
                y = completion_block_matches(
                    bounds, permutation, reference, 3, 2);
                local = (int64_t)conditioned_unary_value(
                            bounds, permutation, target, column) -
                        conditioned_unary_value(
                            bounds, permutation, reference, column) +
                    (int64_t)fixed_prefix_count[column] *
                        (int)(SIDES - column - 1U) *
                        completion_block_matches(
                            bounds, permutation, reference, 2, 2) +
                    (int64_t)fixed_gap[column] * y +
                    (int64_t)fixed_prefix_count[column] *
                        completion_block_matches(
                            bounds, permutation, reference, 2, 3);
                if (g < INT16_MIN || g > INT16_MAX ||
                    local < INT32_MIN || local > INT32_MAX) {
                    return false;
                }
                transition->g = (int16_t)g;
                transition->y = (int8_t)y;
                transition->local = (int32_t)local;
                transition->d_position =
                    bounds->positions[permutation][3];
            }
            {
                unsigned c_position;
                unsigned available = 0;

                for (c_position = 0; c_position < DICE; ++c_position) {
                    unsigned expected =
                        c_position == search->grid[0][column] % DICE ||
                        c_position == search->grid[1][column] % DICE
                        ? 0U : 2U;

                    if (option_count[c_position] != expected) {
                        return false;
                    }
                    if (direction <
                            C_COMPLETION_BASE_SCALAR_DIRECTION_COUNT &&
                        expected != 0U &&
                        completion->transition[track][column]
                                              [c_position][0].g !=
                            completion->transition[track][column]
                                                  [c_position][1].g) {
                        return false;
                    }
                    if (direction == 0U && expected != 0U) {
                        completion->available_c_position[column]
                                                        [available++] =
                            (uint8_t)c_position;
                    }
                }
                if (available != 0U && available != DICE - 2U) {
                    return false;
                }
            }
        }
    }
#if INCREMENTAL_C_COMPLETION_COUPLED
    if (!build_coupled_c_transitions(search)) {
        return false;
    }
#endif
    return true;
}

static bool build_incremental_c_suffixes(struct search *search)
{
    struct incremental_c_completion *completion = &search->c_completion;
    unsigned direction;

    for (direction = 0;
         direction < C_COMPLETION_ACTIVE_DIRECTION_COUNT; ++direction) {
        unsigned state;
        unsigned column;

        for (state = 0; state < EARLY_COMPLETION_STATE_COUNT; ++state) {
            completion->suffix_minimum[direction][SIDES][state] = 0;
            completion->suffix_maximum[direction][SIDES][state] = 0;
        }
        column = SIDES;
        while (column-- > 0) {
            uint8_t fixed_c_position = 0;
            uint8_t fixed_d_position = 0;
            bool fixed = c_completion_static_positions(
                search, column, &fixed_c_position, &fixed_d_position);

            for (state = 0; state < EARLY_COMPLETION_STATE_COUNT; ++state) {
                int old_g = (int)state - (int)EARLY_COMPLETION_G_LIMIT;
                int32_t minimum = INT32_MAX;
                int32_t maximum = INT32_MIN;
                unsigned c_index;

                for (c_index = 0; c_index < DICE - 2U; ++c_index) {
                    unsigned c_position =
                        completion->available_c_position[column][c_index];
                    unsigned option;

                    if (fixed && c_position != fixed_c_position) {
                        continue;
                    }
                    for (option = 0; option < 2U; ++option) {
                        const struct incremental_c_transition *transition =
                            &completion->transition[direction][column]
                                                   [c_position][option];
                        int new_g = old_g + transition->g;
                        unsigned next_state;
                        int64_t candidate_minimum;
                        int64_t candidate_maximum;

                        if (fixed &&
                            transition->d_position != fixed_d_position) {
                            continue;
                        }
                        if (new_g < -(int)EARLY_COMPLETION_G_LIMIT ||
                            new_g > (int)EARLY_COMPLETION_G_LIMIT) {
                            continue;
                        }
                        next_state = (unsigned)(
                            new_g + (int)EARLY_COMPLETION_G_LIMIT);
                        if (completion->suffix_minimum[direction]
                                                      [column + 1U]
                                                      [next_state] ==
                                INT32_MAX) {
                            continue;
                        }
                        candidate_minimum =
                            (int64_t)transition->local +
                            (int64_t)old_g * transition->y +
                            completion->suffix_minimum[direction]
                                                      [column + 1U]
                                                      [next_state];
                        candidate_maximum =
                            (int64_t)transition->local +
                            (int64_t)old_g * transition->y +
                            completion->suffix_maximum[direction]
                                                      [column + 1U]
                                                      [next_state];
                        if (candidate_minimum < INT32_MIN ||
                            candidate_minimum > INT32_MAX ||
                            candidate_maximum < INT32_MIN ||
                            candidate_maximum > INT32_MAX) {
                            return false;
                        }
                        if (candidate_minimum < minimum) {
                            minimum = (int32_t)candidate_minimum;
                        }
                        if (candidate_maximum > maximum) {
                            maximum = (int32_t)candidate_maximum;
                        }
                    }
                }
                completion->suffix_minimum[direction][column][state] =
                    minimum;
                completion->suffix_maximum[direction][column][state] =
                    maximum;
            }
        }
    }
    return true;
}

static bool begin_incremental_c_completion(struct search *search)
{
    struct incremental_c_completion *completion = &search->c_completion;
    unsigned direction;

    if (!build_incremental_c_transitions(search) ||
        !build_incremental_c_suffixes(search)) {
        return false;
    }
    for (direction = 0;
         direction < C_COMPLETION_ACTIVE_SCALAR_DIRECTION_COUNT;
         ++direction) {
        completion->prefix_minimum[direction][0] = 0;
        completion->prefix_maximum[direction][0] = 0;
        completion->prefix_g[direction][0] = 0;
    }
#if C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT > 0
    if (!completion->frontiers_initialized) {
        unsigned boundary;

        for (direction = 0;
             direction < C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT;
             ++direction) {
            for (boundary = 1U; boundary <= SEARCH_COLUMNS; ++boundary) {
                unsigned state;

                for (state = 0; state < EARLY_COMPLETION_STATE_COUNT;
                     ++state) {
                    completion->frontier_minimum[direction][boundary]
                                                [state] = INT32_MAX;
                    completion->frontier_maximum[direction][boundary]
                                                [state] = INT32_MIN;
                }
            }
        }
        completion->frontiers_initialized = true;
    }
    for (direction = 0;
         direction < C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT;
         ++direction) {
        completion->frontier_count[direction][0] = 1U;
        completion->frontier_state[direction][0][0] =
            EARLY_COMPLETION_G_LIMIT;
        completion->frontier_minimum[direction][0]
                                    [EARLY_COMPLETION_G_LIMIT] = 0;
        completion->frontier_maximum[direction][0]
                                    [EARLY_COMPLETION_G_LIMIT] = 0;
    }
#endif
    return true;
}

static bool advance_incremental_c_completion(struct search *search,
                                             unsigned old_boundary)
{
    struct incremental_c_completion *completion = &search->c_completion;
    unsigned new_boundary = old_boundary + 1U;
    unsigned actual_column = old_boundary;
    uint8_t c_position =
        (uint8_t)(search->grid[2][actual_column] % DICE);
    bool fixed_de = actual_column == physical_column(0);
    uint8_t d_position =
        (uint8_t)(search->grid[3][actual_column] % DICE);
    unsigned new_limit = new_boundary * (new_boundary + 1U) / 2U;
    unsigned direction;

    if (old_boundary >= SEARCH_COLUMNS || new_limit >
            EARLY_COMPLETION_G_LIMIT) {
        return false;
    }
    for (direction = 0;
         direction < C_COMPLETION_ACTIVE_SCALAR_DIRECTION_COUNT;
         ++direction) {
        const struct incremental_c_transition *first =
            &completion->transition[direction][actual_column]
                                   [c_position][0];
        const struct incremental_c_transition *second =
            &completion->transition[direction][actual_column]
                                   [c_position][1];
        int old_g = completion->prefix_g[direction][old_boundary];
        int32_t first_delta = first->local + old_g * first->y;
        int32_t second_delta = second->local + old_g * second->y;
        int32_t minimum_delta;
        int32_t maximum_delta;

        if (fixed_de) {
            if (first->d_position == d_position) {
                minimum_delta = first_delta;
                maximum_delta = first_delta;
            } else if (second->d_position == d_position) {
                minimum_delta = second_delta;
                maximum_delta = second_delta;
            } else {
                return false;
            }
        } else {
            minimum_delta = first_delta < second_delta
                ? first_delta : second_delta;
            maximum_delta = first_delta > second_delta
                ? first_delta : second_delta;
        }

        completion->prefix_g[direction][new_boundary] =
            (int16_t)(old_g + first->g);
        completion->prefix_minimum[direction][new_boundary] =
            completion->prefix_minimum[direction][old_boundary] +
            minimum_delta;
        completion->prefix_maximum[direction][new_boundary] =
            completion->prefix_maximum[direction][old_boundary] +
            maximum_delta;
        ++search->c_completion_states;
    }
#if C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT > 0
    for (direction = 0;
         direction < C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT;
         ++direction) {
        unsigned full_direction =
            C_COMPLETION_ACTIVE_SCALAR_DIRECTION_COUNT + direction;
        uint16_t *source_state =
            completion->frontier_state[direction][old_boundary];
        uint16_t *target_state =
            completion->frontier_state[direction][new_boundary];
        uint16_t source_count =
            completion->frontier_count[direction][old_boundary];
        uint16_t old_target_count =
            completion->frontier_count[direction][new_boundary];
        uint16_t target_count = 0;
        unsigned index;
        unsigned fixed_option = 0;
        unsigned options = 2U;
        unsigned option;

        if (fixed_de) {
            const struct incremental_c_transition *first =
                &completion->transition[full_direction][actual_column]
                                       [c_position][0];
            const struct incremental_c_transition *second =
                &completion->transition[full_direction][actual_column]
                                       [c_position][1];

            if (first->d_position == d_position) {
                fixed_option = 0U;
            } else if (second->d_position == d_position) {
                fixed_option = 1U;
            } else {
                return false;
            }
            options = 1U;
        }

        for (index = 0; index < old_target_count; ++index) {
            unsigned state = target_state[index];

            completion->frontier_minimum[direction][new_boundary][state] =
                INT32_MAX;
            completion->frontier_maximum[direction][new_boundary][state] =
                INT32_MIN;
        }
        for (index = 0; index < source_count; ++index) {
            unsigned state = source_state[index];
            int old_g = (int)state - (int)EARLY_COMPLETION_G_LIMIT;

            ++search->c_completion_states;
            for (option = 0; option < options; ++option) {
                const struct incremental_c_transition *transition =
                    &completion->transition[full_direction][actual_column]
                        [c_position][fixed_de ? fixed_option : option];
                int new_g;
                unsigned next_state;
                int32_t delta;
                int32_t candidate_minimum;
                int32_t candidate_maximum;

                new_g = old_g + transition->g;
                next_state = (unsigned)(
                    new_g + (int)EARLY_COMPLETION_G_LIMIT);
                delta = transition->local + old_g * transition->y;
                candidate_minimum =
                    completion->frontier_minimum[direction]
                                                [old_boundary][state] +
                    delta;
                candidate_maximum =
                    completion->frontier_maximum[direction]
                                                [old_boundary][state] +
                    delta;
                if (completion->frontier_minimum[direction][new_boundary]
                                                [next_state] == INT32_MAX) {
                    if (target_count >= EARLY_COMPLETION_STATE_COUNT) {
                        return false;
                    }
                    target_state[target_count++] = (uint16_t)next_state;
                }
                if (candidate_minimum <
                        completion->frontier_minimum[direction][new_boundary]
                                                    [next_state]) {
                    completion->frontier_minimum[direction][new_boundary]
                                                [next_state] =
                        candidate_minimum;
                }
                if (candidate_maximum >
                        completion->frontier_maximum[direction][new_boundary]
                                                    [next_state]) {
                    completion->frontier_maximum[direction][new_boundary]
                                                [next_state] =
                        candidate_maximum;
                }
            }
        }
        completion->frontier_count[direction][new_boundary] = target_count;
    }
#endif
    return true;
}

static void record_c_completion_prune(struct search *search,
                                      unsigned direction)
{
    ++search->c_completion_prunes;
#if INCREMENTAL_C_COMPLETION_COUPLED
    if (direction == C_COMPLETION_PLUS_DIRECTION ||
        direction == C_COMPLETION_MINUS_DIRECTION) {
        ++search->c_completion_coupled_prunes;
    }
#else
    (void)direction;
#endif
}

static bool incremental_c_completion_possible(struct search *search,
                                              unsigned boundary)
{
    const struct incremental_c_completion *completion =
        &search->c_completion;
    unsigned limit = boundary * (boundary + 1U) / 2U;
    unsigned direction;

    ++search->c_completion_checks;
    for (direction = 0;
         direction < C_COMPLETION_ACTIVE_SCALAR_DIRECTION_COUNT;
         ++direction) {
        int g = completion->prefix_g[direction][boundary];
        unsigned state;
        int64_t minimum;
        int64_t maximum;

        if (g < -(int)limit || g > (int)limit) {
            record_c_completion_prune(search, direction);
            return false;
        }
        state = (unsigned)(g + (int)EARLY_COMPLETION_G_LIMIT);
        if (completion->suffix_minimum[direction][boundary][state] ==
                INT32_MAX) {
            record_c_completion_prune(search, direction);
            return false;
        }
        minimum =
            (int64_t)completion->prefix_minimum[direction][boundary] +
            completion->suffix_minimum[direction][boundary][state];
        maximum =
            (int64_t)completion->prefix_maximum[direction][boundary] +
            completion->suffix_maximum[direction][boundary][state];
        if (minimum > 0 || maximum < 0) {
            record_c_completion_prune(search, direction);
            return false;
        }
    }
#if C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT > 0
    for (direction = 0;
         direction < C_COMPLETION_ACTIVE_FRONTIER_DIRECTION_COUNT;
         ++direction) {
        unsigned full_direction =
            C_COMPLETION_ACTIVE_SCALAR_DIRECTION_COUNT + direction;
        uint16_t count = completion->frontier_count[direction][boundary];
        unsigned index;
        bool possible = false;

        for (index = 0; index < count; ++index) {
            unsigned state =
                completion->frontier_state[direction][boundary][index];
            int64_t minimum;
            int64_t maximum;

            if (completion->suffix_minimum[full_direction][boundary][state] ==
                    INT32_MAX) {
                continue;
            }
            minimum =
                (int64_t)completion->frontier_minimum[direction][boundary]
                                                     [state] +
                completion->suffix_minimum[full_direction][boundary][state];
            maximum =
                (int64_t)completion->frontier_maximum[direction][boundary]
                                                     [state] +
                completion->suffix_maximum[full_direction][boundary][state];
            if (minimum <= 0 && maximum >= 0) {
                possible = true;
                break;
            }
        }
        if (!possible) {
            record_c_completion_prune(search, full_direction);
            return false;
        }
    }
#endif
    return true;
}
#endif

static bool conditioned_completion_possible(struct search *search,
                                             unsigned row)
{
    const struct conditioned_completion_bounds *bounds =
        &search->completion_bounds;
    uint8_t base_index[SIDES];
    uint8_t alternate_index[SIDES];
    int32_t reference_base[SIDES];
    int32_t reference_alternate[SIDES];
    unsigned column;
    unsigned direction;

    ++search->completion_checks;
    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        unsigned actual_column = ordered_column(search, row, column);
        unsigned candidates = search->candidate_mask[row][column];
        unsigned first;

        if (candidates == 0U) {
            ++search->completion_prunes;
            return false;
        }
        first = (unsigned)__builtin_ctz(candidates);
        candidates &= candidates - 1U;
        base_index[actual_column] = (uint8_t)
            completion_actual_column_permutation_index(
                search, row, actual_column, first);
        alternate_index[actual_column] = base_index[actual_column];
#if MIRROR_COLUMNS > 0
        if (column_has_mirror(actual_column)) {
            unsigned mirror_column = SIDES - actual_column - 1U;

            base_index[mirror_column] = (uint8_t)
                completion_actual_column_permutation_index(
                    search, row, mirror_column, first);
            alternate_index[mirror_column] = base_index[mirror_column];
        }
#endif
        if (candidates != 0U) {
            unsigned second = (unsigned)__builtin_ctz(candidates);

            if ((candidates & (candidates - 1U)) != 0U) {
                return true;
            }
            alternate_index[actual_column] = (uint8_t)
                completion_actual_column_permutation_index(
                    search, row, actual_column, second);
#if MIRROR_COLUMNS > 0
            if (column_has_mirror(actual_column)) {
                unsigned mirror_column = SIDES - actual_column - 1U;

                alternate_index[mirror_column] = (uint8_t)
                    completion_actual_column_permutation_index(
                        search, row, mirror_column, second);
            }
#endif
        }
    }
    conditioned_correlation_values(
        bounds, bounds->word[0], base_index, alternate_index,
        reference_base, reference_alternate);

    for (direction = 0; direction < CONDITIONED_DIRECTION_COUNT;
         ++direction) {
        int32_t direction_base[SIDES];
        int32_t direction_alternate[SIDES];
        int64_t minimum = 0;
        int64_t maximum = 0;

        conditioned_correlation_values(
            bounds, bounds->word[direction + 1U], base_index,
            alternate_index, direction_base, direction_alternate);
        for (column = 0; column < SEARCH_COLUMNS; ++column) {
            unsigned actual_column = ordered_column(search, row, column);
            int64_t first =
#if DICE == 4
                bounds->unary[direction][actual_column]
                             [base_index[actual_column]] +
#else
                conditioned_unary_value(
                    bounds, base_index[actual_column],
                    bounds->word[direction + 1U], actual_column) -
                conditioned_unary_value(
                    bounds, base_index[actual_column], bounds->word[0],
                    actual_column) +
#endif
                direction_base[actual_column] -
                reference_base[actual_column];
            int64_t second =
#if DICE == 4
                bounds->unary[direction][actual_column]
                             [alternate_index[actual_column]] +
#else
                conditioned_unary_value(
                    bounds, alternate_index[actual_column],
                    bounds->word[direction + 1U], actual_column) -
                conditioned_unary_value(
                    bounds, alternate_index[actual_column],
                    bounds->word[0], actual_column) +
#endif
                direction_alternate[actual_column] -
                reference_alternate[actual_column];

#if MIRROR_COLUMNS > 0
            if (column_has_mirror(actual_column)) {
                unsigned mirror_column = SIDES - actual_column - 1U;

                first +=
#if DICE == 4
                    bounds->unary[direction][mirror_column]
                                 [base_index[mirror_column]] +
#else
                    conditioned_unary_value(
                        bounds, base_index[mirror_column],
                        bounds->word[direction + 1U], mirror_column) -
                    conditioned_unary_value(
                        bounds, base_index[mirror_column], bounds->word[0],
                        mirror_column) +
#endif
                    direction_base[mirror_column] -
                    reference_base[mirror_column];
                second +=
#if DICE == 4
                    bounds->unary[direction][mirror_column]
                                 [alternate_index[mirror_column]] +
#else
                    conditioned_unary_value(
                        bounds, alternate_index[mirror_column],
                        bounds->word[direction + 1U], mirror_column) -
                    conditioned_unary_value(
                        bounds, alternate_index[mirror_column],
                        bounds->word[0], mirror_column) +
#endif
                    direction_alternate[mirror_column] -
                    reference_alternate[mirror_column];
            }
#endif

            minimum += first < second ? first : second;
            maximum += first > second ? first : second;
        }
        if (minimum > 0 || maximum < 0) {
            ++search->completion_prunes;
            return false;
        }
    }
    return true;
}
#endif

static uint64_t mitm_hash_coefficient(unsigned coordinate)
{
    uint64_t value = (uint64_t)(coordinate + 1U) *
        UINT64_C(0x9e3779b97f4a7c15);

    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return (value ^ (value >> 31U)) | UINT64_C(1);
}

static uint64_t row2_mitm_vector_hash(
    const int64_t vector[ROW2_MITM_VECTOR_WIDTH])
{
    uint64_t hash = 0;
    unsigned coordinate;

    for (coordinate = 0; coordinate < ROW2_MITM_VECTOR_WIDTH;
         ++coordinate) {
        hash += (uint64_t)vector[coordinate] *
            mitm_hash_coefficient(coordinate);
    }
    return hash;
}

static void row2_choice_vector(const struct search *search, unsigned row,
                               unsigned column, unsigned candidate,
                               int64_t vector[ROW2_MITM_VECTOR_WIDTH])
{
    const struct additive_perm_bounds *bounds =
        &search->additive_permutations;
    unsigned actual_column = ordered_column(search, row, column);
    unsigned permutation_count = bounds->permutation_count[row];
    unsigned permutation_coordinates = permutation_count - 1U;
    unsigned coordinate;

    for (coordinate = 0; coordinate < permutation_coordinates;
         ++coordinate) {
        vector[coordinate] = (int64_t)additive_perm_contribution_at(
            search, bounds, row, candidate, actual_column, coordinate);
    }
    for (coordinate = 0; coordinate < DICE - 1U; ++coordinate) {
        vector[permutation_coordinates + coordinate] =
            (int64_t)choice_contribution(
                search, row, candidate, column, coordinate);
    }
}

/*
 * Solve the final binary row as an exact meet-in-the-middle join.  A linear
 * 64-bit fingerprint is only an index: every hash hit is verified against
 * all exact coordinates, so collisions can add work but never affect
 * correctness.
 */
static void solve_row2_meet_in_middle(struct search *search, unsigned row)
{
    const struct additive_perm_bounds *bounds =
        &search->additive_permutations;
    unsigned base_candidate[SEARCH_COLUMNS];
    unsigned alternate_candidate[SEARCH_COLUMNS];
    unsigned variable_position[SEARCH_COLUMNS];
    int64_t delta[SEARCH_COLUMNS][ROW2_MITM_VECTOR_WIDTH];
    uint64_t delta_hash[SEARCH_COLUMNS];
    int64_t base[ROW2_MITM_VECTOR_WIDTH] = {0};
    int64_t target[ROW2_MITM_VECTOR_WIDTH];
    int64_t target_delta[ROW2_MITM_VECTOR_WIDTH];
    unsigned variable_count = 0;
    unsigned left_variables;
    unsigned right_variables;
    uint32_t left_count;
    uint32_t right_count;
    unsigned column;
    unsigned permutation_coordinates =
        bounds->permutation_count[row] - 1U;
    unsigned coordinate;
    uint64_t target_hash;

    ++search->mitm_solves;

    for (coordinate = 0; coordinate < permutation_coordinates;
         ++coordinate) {
        target[coordinate] = (int64_t)bounds->goal[row];
    }
    for (coordinate = permutation_coordinates;
         coordinate < ROW2_MITM_VECTOR_WIDTH;
         ++coordinate) {
        target[coordinate] = (int64_t)search->place_goal;
    }
    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        unsigned candidates = search->candidate_mask[row][column];
        unsigned first;
        int64_t first_vector[ROW2_MITM_VECTOR_WIDTH];

        if (candidates == 0) {
            return;
        }
        first = (unsigned)__builtin_ctz(candidates);
        candidates &= candidates - 1U;
        base_candidate[column] = first;
        alternate_candidate[column] = first;
        row2_choice_vector(search, row, column, first, first_vector);
        for (coordinate = 0; coordinate < ROW2_MITM_VECTOR_WIDTH;
             ++coordinate) {
            base[coordinate] += first_vector[coordinate];
        }
        if (candidates != 0) {
            unsigned second = (unsigned)__builtin_ctz(candidates);
            int64_t second_vector[ROW2_MITM_VECTOR_WIDTH];

            if ((candidates & (candidates - 1U)) != 0) {
                return;
            }
            alternate_candidate[column] = second;
            variable_position[variable_count] = column;
            row2_choice_vector(search, row, column, second, second_vector);
            for (coordinate = 0; coordinate < ROW2_MITM_VECTOR_WIDTH;
                 ++coordinate) {
                delta[variable_count][coordinate] =
                    second_vector[coordinate] - first_vector[coordinate];
            }
            delta_hash[variable_count] =
                row2_mitm_vector_hash(delta[variable_count]);
            ++variable_count;
        }
    }
    left_variables = variable_count / 2U;
    right_variables = variable_count - left_variables;
    if (left_variables > ROW2_MITM_MAX_LEFT_VARIABLES ||
        right_variables > ROW2_MITM_MAX_LEFT_VARIABLES) {
        return;
    }
    left_count = UINT32_C(1) << left_variables;
    right_count = UINT32_C(1) << right_variables;
    memset(search->row2_mitm_head, 0xff, sizeof(search->row2_mitm_head));
    {
        uint64_t current_hash = 0;
        uint32_t previous_gray = 0;
        uint32_t ordinal;

        for (ordinal = 0; ordinal < left_count; ++ordinal) {
            uint32_t gray = ordinal ^ (ordinal >> 1U);
            uint32_t bucket;
            struct row2_mitm_entry *entry =
                &search->row2_mitm_entry[ordinal];

            if (ordinal != 0) {
                uint32_t changed = gray ^ previous_gray;
                unsigned variable = (unsigned)__builtin_ctz(changed);

                if ((gray & changed) != 0) {
                    current_hash += delta_hash[variable];
                } else {
                    current_hash -= delta_hash[variable];
                }
            }
            bucket = (uint32_t)current_hash &
                (ROW2_MITM_HASH_CAPACITY - 1U);
            entry->hash = current_hash;
            entry->choices = gray;
            entry->next = search->row2_mitm_head[bucket];
            search->row2_mitm_head[bucket] = ordinal;
            previous_gray = gray;
        }
    }
    for (coordinate = 0; coordinate < ROW2_MITM_VECTOR_WIDTH;
         ++coordinate) {
        target_delta[coordinate] = target[coordinate] - base[coordinate];
    }
    target_hash = row2_mitm_vector_hash(target_delta);
    {
        uint64_t right_hash = 0;
        uint32_t previous_gray = 0;
        uint32_t ordinal;

        for (ordinal = 0; ordinal < right_count; ++ordinal) {
            uint32_t right_gray = ordinal ^ (ordinal >> 1U);
            uint64_t needed_hash;
            uint32_t entry_index;

            if (ordinal != 0) {
                uint32_t changed = right_gray ^ previous_gray;
                unsigned variable = left_variables +
                    (unsigned)__builtin_ctz(changed);

                if ((right_gray & changed) != 0) {
                    right_hash += delta_hash[variable];
                } else {
                    right_hash -= delta_hash[variable];
                }
            }
            needed_hash = target_hash - right_hash;
            entry_index = search->row2_mitm_head[
                (uint32_t)needed_hash & (ROW2_MITM_HASH_CAPACITY - 1U)];
            while (entry_index != UINT32_MAX) {
                const struct row2_mitm_entry *entry =
                    &search->row2_mitm_entry[entry_index];

                if (entry->hash == needed_hash) {
                    int64_t total[ROW2_MITM_VECTOR_WIDTH];
                    unsigned variable;
                    bool exact = true;

                    memcpy(total, base, sizeof(total));
                    for (variable = 0; variable < left_variables;
                         ++variable) {
                        if ((entry->choices & (1U << variable)) != 0) {
                            for (coordinate = 0;
                                 coordinate < ROW2_MITM_VECTOR_WIDTH;
                                 ++coordinate) {
                                total[coordinate] +=
                                    delta[variable][coordinate];
                            }
                        }
                    }
                    for (variable = 0; variable < right_variables;
                         ++variable) {
                        if ((right_gray & (1U << variable)) != 0) {
                            unsigned index = left_variables + variable;

                            for (coordinate = 0;
                                 coordinate < ROW2_MITM_VECTOR_WIDTH;
                                 ++coordinate) {
                                total[coordinate] +=
                                    delta[index][coordinate];
                            }
                        }
                    }
                    for (coordinate = 0;
                         coordinate < ROW2_MITM_VECTOR_WIDTH; ++coordinate) {
                        if (total[coordinate] != target[coordinate]) {
                            exact = false;
                            break;
                        }
                    }
                    if (exact) {
                        unsigned selected[SEARCH_COLUMNS];

                        memcpy(selected, base_candidate, sizeof(selected));
                        for (variable = 0; variable < left_variables;
                             ++variable) {
                            if ((entry->choices & (1U << variable)) != 0) {
                                unsigned position =
                                    variable_position[variable];

                                selected[position] =
                                    alternate_candidate[position];
                            }
                        }
                        for (variable = 0; variable < right_variables;
                             ++variable) {
                            if ((right_gray & (1U << variable)) != 0) {
                                unsigned position = variable_position[
                                    left_variables + variable];

                                selected[position] =
                                    alternate_candidate[position];
                            }
                        }
                        for (column = 0; column < SEARCH_COLUMNS; ++column) {
                            apply_choice(
                                search, row, column, selected[column]);
                        }
                        if (completed_row_is_fair(search, row)) {
                            accept_configuration(search);
                        }
                        column = SEARCH_COLUMNS;
                        while (column-- > 0) {
                            undo_choice(
                                search, row, column, selected[column]);
                        }
                        if (atomic_load_explicit(
                                &search->shared->stop,
                                memory_order_relaxed)) {
                            return;
                        }
                    }
                }
                entry_index = entry->next;
            }
            previous_gray = right_gray;
        }
    }
}
#endif

#if ROW1_MITM_ACTIVE
static void search_row(struct search *search, unsigned row, unsigned column);

static uint64_t row1_mitm_vector_hash(
    const int64_t vector[ROW1_MITM_VECTOR_WIDTH])
{
    uint64_t hash = 0;
    unsigned coordinate;

    for (coordinate = 0; coordinate < ROW1_MITM_VECTOR_WIDTH;
         ++coordinate) {
        hash += (uint64_t)vector[coordinate] *
            mitm_hash_coefficient(coordinate);
    }
    return hash;
}

static void row1_choice_vector(const struct search *search, unsigned row,
                               unsigned column, unsigned candidate,
                               int64_t vector[ROW1_MITM_VECTOR_WIDTH])
{
    unsigned place;

#if FULL_MIRROR
    for (place = 0; place < 3U; ++place) {
        vector[place] = (int64_t)choice_contribution(
            search, row, candidate, column, place);
    }
#else
    unsigned actual_column = ordered_column(search, row, column);

    vector[0] = pair_choice_win(
        search, candidate, 0, actual_column);
    for (place = 0; place < 3U; ++place) {
        vector[place + 1U] = (int64_t)choice_contribution(
            search, row, candidate, column, place);
    }
#endif
}

static void solve_row1_meet_in_middle(struct search *search, unsigned row)
{
    unsigned base_candidate[SEARCH_COLUMNS];
    unsigned variable_position[SEARCH_COLUMNS];
    unsigned option_candidate[SEARCH_COLUMNS][3];
    unsigned option_count[SEARCH_COLUMNS];
    int64_t delta[SEARCH_COLUMNS][3][ROW1_MITM_VECTOR_WIDTH];
    uint64_t delta_hash[SEARCH_COLUMNS][3];
    int64_t base[ROW1_MITM_VECTOR_WIDTH] = {0};
    int64_t target[ROW1_MITM_VECTOR_WIDTH] = {
#if FULL_MIRROR
        search->place_goal, search->place_goal, search->place_goal,
#else
        PAIR_GOAL, search->place_goal, search->place_goal,
        search->place_goal,
#endif
    };
    int64_t target_delta[ROW1_MITM_VECTOR_WIDTH];
    unsigned variable_count = 0;
    unsigned left_variables;
    unsigned right_variables;
    uint32_t left_count = 1;
    uint32_t right_count = 1;
    uint64_t target_hash;
    unsigned column;
    unsigned coordinate;

    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        unsigned candidates = search->candidate_mask[row][column];
        unsigned first;
        int64_t first_vector[ROW1_MITM_VECTOR_WIDTH];

        if (candidates == 0) {
            return;
        }
        first = (unsigned)__builtin_ctz(candidates);
        candidates &= candidates - 1U;
        base_candidate[column] = first;
        row1_choice_vector(search, row, column, first, first_vector);
        for (coordinate = 0; coordinate < ROW1_MITM_VECTOR_WIDTH;
             ++coordinate) {
            base[coordinate] += first_vector[coordinate];
        }
        if (candidates != 0) {
            unsigned count = 1;

            variable_position[variable_count] = column;
            option_candidate[variable_count][0] = first;
            memset(delta[variable_count][0], 0,
                   sizeof(delta[variable_count][0]));
            delta_hash[variable_count][0] = 0;
            while (candidates != 0 && count < 3U) {
                unsigned candidate = (unsigned)__builtin_ctz(candidates);
                int64_t vector[ROW1_MITM_VECTOR_WIDTH];

                candidates &= candidates - 1U;
                option_candidate[variable_count][count] = candidate;
                row1_choice_vector(
                    search, row, column, candidate, vector);
                for (coordinate = 0;
                     coordinate < ROW1_MITM_VECTOR_WIDTH; ++coordinate) {
                    delta[variable_count][count][coordinate] =
                        vector[coordinate] - first_vector[coordinate];
                }
                delta_hash[variable_count][count] =
                    row1_mitm_vector_hash(delta[variable_count][count]);
                ++count;
            }
            if (candidates != 0) {
                return;
            }
            option_count[variable_count] = count;
            ++variable_count;
        }
    }
    left_variables = variable_count / 2U;
    right_variables = variable_count - left_variables;
    for (column = 0; column < left_variables; ++column) {
        left_count *= option_count[column];
    }
    for (column = left_variables; column < variable_count; ++column) {
        right_count *= option_count[column];
    }
    if (left_count > ROW1_MITM_LEFT_CAPACITY ||
        left_count * 2U > ROW1_MITM_HASH_CAPACITY) {
        return;
    }
    memset(search->row1_mitm_head, 0xff, sizeof(search->row1_mitm_head));
    {
        unsigned digit[SEARCH_COLUMNS] = {0};
        uint32_t packed = 0;
        uint64_t current_hash = 0;
        uint32_t ordinal;

        for (ordinal = 0; ordinal < left_count; ++ordinal) {
            uint32_t bucket = (uint32_t)current_hash &
                (ROW1_MITM_HASH_CAPACITY - 1U);
            struct row1_mitm_entry *entry =
                &search->row1_mitm_entry[ordinal];

            entry->hash = current_hash;
            entry->choices = packed;
            entry->next = search->row1_mitm_head[bucket];
            search->row1_mitm_head[bucket] = ordinal;
            if (ordinal + 1U < left_count) {
                unsigned variable = 0;

                for (;;) {
                    unsigned old = digit[variable];
                    unsigned next = old + 1U;

                    current_hash -= delta_hash[variable][old];
                    if (next == option_count[variable]) {
                        next = 0;
                    }
                    digit[variable] = next;
                    current_hash += delta_hash[variable][next];
                    packed &= ~(UINT32_C(3) << (2U * variable));
                    packed |= (uint32_t)next << (2U * variable);
                    if (next != 0) {
                        break;
                    }
                    ++variable;
                }
            }
        }
    }
    for (coordinate = 0; coordinate < ROW1_MITM_VECTOR_WIDTH;
         ++coordinate) {
        target_delta[coordinate] = target[coordinate] - base[coordinate];
    }
    target_hash = row1_mitm_vector_hash(target_delta);
    {
        unsigned digit[SEARCH_COLUMNS] = {0};
        uint32_t packed = 0;
        uint64_t current_hash = 0;
        uint32_t ordinal;

        for (ordinal = 0; ordinal < right_count; ++ordinal) {
            uint64_t needed_hash = target_hash - current_hash;
            uint32_t entry_index = search->row1_mitm_head[
                (uint32_t)needed_hash & (ROW1_MITM_HASH_CAPACITY - 1U)];

            while (entry_index != UINT32_MAX) {
                const struct row1_mitm_entry *entry =
                    &search->row1_mitm_entry[entry_index];

                if (entry->hash == needed_hash) {
                    int64_t total[ROW1_MITM_VECTOR_WIDTH];
                    unsigned variable;
                    bool exact = true;

                    memcpy(total, base, sizeof(total));
                    for (variable = 0; variable < left_variables;
                         ++variable) {
                        unsigned choice = (entry->choices >>
                            (2U * variable)) & 3U;

                        for (coordinate = 0;
                             coordinate < ROW1_MITM_VECTOR_WIDTH;
                             ++coordinate) {
                            total[coordinate] +=
                                delta[variable][choice][coordinate];
                        }
                    }
                    for (variable = 0; variable < right_variables;
                         ++variable) {
                        unsigned choice = (packed >> (2U * variable)) & 3U;
                        unsigned index = left_variables + variable;

                        for (coordinate = 0;
                             coordinate < ROW1_MITM_VECTOR_WIDTH;
                             ++coordinate) {
                            total[coordinate] +=
                                delta[index][choice][coordinate];
                        }
                    }
                    for (coordinate = 0;
                         coordinate < ROW1_MITM_VECTOR_WIDTH; ++coordinate) {
                        if (total[coordinate] != target[coordinate]) {
                            exact = false;
                            break;
                        }
                    }
                    if (exact) {
                        unsigned selected[SEARCH_COLUMNS];

                        memcpy(selected, base_candidate, sizeof(selected));
                        for (variable = 0; variable < left_variables;
                             ++variable) {
                            unsigned choice = (entry->choices >>
                                (2U * variable)) & 3U;
                            unsigned position =
                                variable_position[variable];

                            selected[position] =
                                option_candidate[variable][choice];
                        }
                        for (variable = 0; variable < right_variables;
                             ++variable) {
                            unsigned choice = (packed >>
                                (2U * variable)) & 3U;
                            unsigned index = left_variables + variable;
                            unsigned position = variable_position[index];

                            selected[position] =
                                option_candidate[index][choice];
                        }
                        for (column = 0; column < SEARCH_COLUMNS; ++column) {
                            apply_choice(
                                search, row, column, selected[column]);
                        }
                        if (completed_row_is_fair(search, row)) {
                            search_row(search, row + 1U, 0);
                        }
                        column = SEARCH_COLUMNS;
                        while (column-- > 0) {
                            undo_choice(
                                search, row, column, selected[column]);
                        }
                        if (atomic_load_explicit(
                                &search->shared->stop,
                                memory_order_relaxed)) {
                            return;
                        }
                    }
                }
                entry_index = entry->next;
            }
            if (ordinal + 1U < right_count) {
                unsigned local = 0;

                for (;;) {
                    unsigned variable = left_variables + local;
                    unsigned old = digit[local];
                    unsigned next = old + 1U;

                    current_hash -= delta_hash[variable][old];
                    if (next == option_count[variable]) {
                        next = 0;
                    }
                    digit[local] = next;
                    current_hash += delta_hash[variable][next];
                    packed &= ~(UINT32_C(3) << (2U * local));
                    packed |= (uint32_t)next << (2U * local);
                    if (next != 0) {
                        break;
                    }
                    ++local;
                }
            }
        }
    }
}
#endif

/*
 * Fill one die in its planned column order.  Each branch swaps one face into
 * place, recurses, then restores that swap and its additive tallies.  There
 * are no configuration or tally copies anywhere in the recursion.
 *
 * Once DICE-1 dice meet the goal, the remaining die is forced and place-fair:
 * the totals over all dice are fixed.  accept_configuration verifies this.
 */
static void search_row(struct search *search, unsigned row, unsigned column)
{
    unsigned candidate;
    if (atomic_load_explicit(&search->shared->stop, memory_order_relaxed)) {
        return;
    }
    ++search->nodes;
    if ((search->nodes & PROGRESS_CHECK_MASK) == 0) {
        publish_worker_stats(search);
    }

    if (column == 0) {
#if ROW2_MITM_ACTIVE
        if ((row == DICE - 2U
#if ROW1_MITM_ACTIVE
             || row == 1U
#endif
            ) && !search->template_active) {
            prepare_mitm_columns(search, row);
        } else
#endif
        {
#if INCREMENTAL_C_COMPLETION_ACTIVE
            if (row == 2U && !search->template_active) {
                prepare_incremental_c_columns(search, row);
            } else
#endif
            {
            plan_column_order(search, row);
            }
            build_bounds(search, row);
        }
    }
#if ROW1_MITM_ACTIVE
    if (row == 1U && column == 0 && !search->template_active) {
        solve_row1_meet_in_middle(search, row);
        return;
    }
#endif
#if ROW2_MITM_ACTIVE
    if (row == DICE - 2U && column == 0 && !search->template_active) {
        build_additive_perm_bounds(search, row);
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
        if (!conditioned_completion_possible(search, row)) {
            return;
        }
#endif
        solve_row2_meet_in_middle(search, row);
        return;
    }
#endif
#if !FULL_MIRROR
    if (row != 0 && column == PAIR_BOUND_START) {
        initialize_pair_wins(search, row, column);
    }
#endif
    if (!bounds_allow_goal(search, row, column)) {
        ++search->bound_prunes;
        return;
    }
    if (!direction_bounds_allow_goal(search, row, column)) {
        ++search->linear_place_prunes;
        return;
    }
    if (!pair_bounds_allow_goal(search, row, column)) {
        ++search->pair_bound_prunes;
        return;
    }
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    if (column == 0 && row >= 2U) {
        build_additive_perm_bounds(search, row);
    }
    if (!additive_perm_bounds_allow_goal(search, row, column)) {
        ++search->additive_perm_prunes;
        return;
    }
#if ADDITIVE_PERM_LINEAR_ACTIVE
    if (!additive_perm_direction_bounds_allow_goal(search, row, column)) {
        ++search->linear_perm_prunes;
        return;
    }
#endif
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
    if (row == 2U && !search->template_active) {
        unsigned boundary = incremental_c_boundary(column);
        bool initialized = true;

        if (column == 0U) {
            initialized = begin_incremental_c_completion(search);
        } else {
            unsigned previous_boundary =
                incremental_c_boundary(column - 1U);

            if (boundary != previous_boundary) {
                if (boundary != previous_boundary + 1U ||
                    ordered_column(search, row, column - 1U) !=
                        previous_boundary) {
                    initialized = false;
                } else {
                    initialized = advance_incremental_c_completion(
                        search, previous_boundary);
                }
            }
        }
        if (!initialized) {
            atomic_store_explicit(&search->shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&search->shared->stop, true,
                                  memory_order_relaxed);
            return;
        }
        if ((column == 0U ||
             boundary != incremental_c_boundary(column - 1U)) &&
            !incremental_c_completion_possible(search, boundary)) {
            return;
        }
    }
#endif
    if (column == SEARCH_COLUMNS) {
        if (!completed_row_is_fair(search, row)) {
#if !PERM_ONLY
            ++search->prefix_place_prunes;
#endif
            return;
        }
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
        if (row == 1U && !search->template_active &&
            !early_conditioned_completion_possible(search)) {
            return;
        }
#endif
        if (row + 1U < DICE - 1U) {
            search_row(search, row + 1U, 0);
        } else {
            /*
             * The last die is forced rather than visited by search_row().
             * Pair bounds certified every pair among the built dice.  Once
             * full place fairness is verified, expected-rank balance forces
             * every remaining matchup with the last die to balance as well.
             */
            accept_configuration(search);
        }
        return;
    }

    if (search->template_active) {
        unsigned mask = search->candidate_mask[row][column];

        while (mask != 0) {
            candidate = (unsigned)__builtin_ctz(mask);
            mask &= mask - 1U;
            apply_choice(search, row, column, candidate);
            search_row(search, row, column + 1U);
            undo_choice(search, row, column, candidate);
            if (atomic_load_explicit(&search->shared->stop,
                                     memory_order_relaxed)) {
                return;
            }
        }
        return;
    }

    /* Give the compiler a separate, provably self-swapping first branch. */
    apply_choice(search, row, column, row);
    search_row(search, row, column + 1U);
    undo_choice(search, row, column, row);
    if (column == 0 ||
        atomic_load_explicit(&search->shared->stop, memory_order_relaxed)) {
        return;
    }

    for (candidate = row + 1U; candidate < DICE; ++candidate) {
        apply_choice(search, row, column, candidate);
        search_row(search, row, column + 1U);
        undo_choice(search, row, column, candidate);

        if (atomic_load_explicit(&search->shared->stop,
                                 memory_order_relaxed)) {
            return;
        }
    }
}

static void prepare_template_search(struct search *search,
                                    unsigned *start_row)
{
    unsigned row = 0;

    if (!search->template_active) {
        plan_column_order(search, 0);
        build_bounds(search, 0);
        *start_row = 0;
        return;
    }

    while (row + 1U < DICE &&
           search->fixed_row_count[row] == SEARCH_COLUMNS) {
        unsigned column;

        plan_column_order(search, row);
        build_bounds(search, row);
#if !FULL_MIRROR
        if (row != 0) {
            initialize_pair_wins(search, row, 0);
        }
#endif
#if ADDITIVE_PERM_BOUNDS_ACTIVE
        if (row >= 2U) {
            build_additive_perm_bounds(search, row);
        }
#endif
        for (column = 0; column < SEARCH_COLUMNS; ++column) {
            apply_choice(search, row, column, row);
        }
        if (!bounds_allow_goal(search, row, SEARCH_COLUMNS) ||
            !pair_bounds_allow_goal(search, row, SEARCH_COLUMNS)
#if ADDITIVE_PERM_BOUNDS_ACTIVE
            || !additive_perm_bounds_allow_goal(
                search, row, SEARCH_COLUMNS)
#endif
            || !completed_row_is_fair(search, row)) {
            search->template_possible = false;
            *start_row = row;
            return;
        }
        ++row;
    }
    *start_row = row;
    if (row + 1U < DICE) {
        plan_column_order(search, row);
        build_bounds(search, row);
#if !FULL_MIRROR
        if (row != 0) {
            initialize_pair_wins(search, row, 0);
        }
#endif
#if ADDITIVE_PERM_BOUNDS_ACTIVE
        if (row >= 2U) {
            build_additive_perm_bounds(search, row);
        }
#endif
    }
}

static bool initialize_search(struct search *search,
                              const struct options *options,
                              unsigned *start_row)
{
    memset(search, 0, sizeof(*search));

    if (!integer_power(SIDES, DICE, &search->outcome_count)) {
        fprintf(stderr, "SIDES^DICE does not fit in a 64-bit tally.\n");
        return false;
    }
    search->place_goal = search->outcome_count / DICE;
    search->permutation_fairness_possible =
        search->outcome_count % DICE_FACTORIAL == 0;
    search->linear_place_bounds_possible =
        search->outcome_count <= (uint64_t)INT64_MAX;

    if (!configure_template(search, options)) {
        return false;
    }
    if (!build_contributions(search)) {
        fprintf(stderr, "Place contributions overflowed a 64-bit tally.\n");
        return false;
    }
    if (!initialize_perm_counter(&search->permutations)) {
        fprintf(stderr, "Unable to initialize the permutation counter.\n");
        return false;
    }
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    if (!initialize_additive_perm_bounds(search)) {
        fprintf(stderr, "Unable to initialize additive permutation bounds.\n");
        return false;
    }
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    if (!initialize_conditioned_completion_bounds(search)) {
        fprintf(stderr, "Unable to initialize conditioned completion bounds.\n");
        return false;
    }
#endif
    prepare_template_search(search, start_row);
    return true;
}

struct totals {
    uint64_t nodes;
#if ROW2_MITM_ACTIVE
    uint64_t mitm_solves;
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    uint64_t completion_checks;
    uint64_t completion_prunes;
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    uint64_t early_completion_checks;
    uint64_t early_completion_prunes;
    uint64_t early_completion_states;
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
    uint64_t c_completion_checks;
    uint64_t c_completion_prunes;
#if INCREMENTAL_C_COMPLETION_COUPLED
    uint64_t c_completion_coupled_prunes;
#endif
    uint64_t c_completion_states;
#endif
#endif
    uint64_t bound_prunes;
    uint64_t linear_place_prunes;
    uint64_t pair_bound_prunes;
    uint64_t prefix_place_prunes;
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    uint64_t additive_perm_prunes;
#if ADDITIVE_PERM_LINEAR_ACTIVE
    uint64_t linear_perm_prunes;
#endif
#endif
    uint64_t all_subset_place_prunes;
    uint64_t all_subset_place_fair_count;
    uint64_t permutation_fair_count;
};

#define SI_COUNT_TEXT_SIZE 24

/* Keep exact small counts, then use four significant digits and an SI suffix. */
static void format_si_count(char text[SI_COUNT_TEXT_SIZE], uint64_t value)
{
    static const char suffixes[] = "kMGTPE";
    uint64_t divisor = UINT64_C(1000);
    uint64_t factor;
    uint64_t rounded;
    unsigned suffix = 0;

    if (value <= UINT64_C(9999)) {
        snprintf(text, SI_COUNT_TEXT_SIZE, "%" PRIu64, value);
        return;
    }

    while (suffix + 1U < sizeof(suffixes) - 1U &&
           value / divisor >= UINT64_C(1000)) {
        divisor *= UINT64_C(1000);
        ++suffix;
    }

    for (;;) {
        uint64_t whole = value / divisor;
        uint64_t remainder = value % divisor;
        uint64_t unit;

        factor = whole >= 100U ? UINT64_C(10) :
            (whole >= 10U ? UINT64_C(100) : UINT64_C(1000));
        for (;;) {
            unit = divisor / factor;
            rounded = whole * factor +
                (remainder + unit / 2U) / unit;
            if (rounded < UINT64_C(10000) || factor == UINT64_C(10)) {
                break;
            }
            factor /= UINT64_C(10);
        }
        if (rounded < UINT64_C(10000) ||
            suffix + 1U >= sizeof(suffixes) - 1U) {
            break;
        }
        divisor *= UINT64_C(1000);
        ++suffix;
    }

    if (factor == UINT64_C(1000)) {
        snprintf(text, SI_COUNT_TEXT_SIZE, "%" PRIu64 ".%03" PRIu64 "%c",
                 rounded / factor, rounded % factor, suffixes[suffix]);
    } else if (factor == UINT64_C(100)) {
        snprintf(text, SI_COUNT_TEXT_SIZE, "%" PRIu64 ".%02" PRIu64 "%c",
                 rounded / factor, rounded % factor, suffixes[suffix]);
    } else {
        snprintf(text, SI_COUNT_TEXT_SIZE, "%" PRIu64 ".%01" PRIu64 "%c",
                 rounded / factor, rounded % factor, suffixes[suffix]);
    }
}

static struct totals collect_totals(const struct worker *workers,
                                    unsigned thread_count)
{
    struct totals totals = {0};
    unsigned i;

    for (i = 0; i < thread_count; ++i) {
        totals.nodes += atomic_load_explicit(&workers[i].stats.nodes,
                                             memory_order_relaxed);
#if ROW2_MITM_ACTIVE
        totals.mitm_solves += atomic_load_explicit(
            &workers[i].stats.mitm_solves, memory_order_relaxed);
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
        totals.completion_checks += atomic_load_explicit(
            &workers[i].stats.completion_checks, memory_order_relaxed);
        totals.completion_prunes += atomic_load_explicit(
            &workers[i].stats.completion_prunes, memory_order_relaxed);
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
        totals.early_completion_checks += atomic_load_explicit(
            &workers[i].stats.early_completion_checks,
            memory_order_relaxed);
        totals.early_completion_prunes += atomic_load_explicit(
            &workers[i].stats.early_completion_prunes,
            memory_order_relaxed);
        totals.early_completion_states += atomic_load_explicit(
            &workers[i].stats.early_completion_states,
            memory_order_relaxed);
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
        totals.c_completion_checks += atomic_load_explicit(
            &workers[i].stats.c_completion_checks,
            memory_order_relaxed);
        totals.c_completion_prunes += atomic_load_explicit(
            &workers[i].stats.c_completion_prunes,
            memory_order_relaxed);
#if INCREMENTAL_C_COMPLETION_COUPLED
        totals.c_completion_coupled_prunes += atomic_load_explicit(
            &workers[i].stats.c_completion_coupled_prunes,
            memory_order_relaxed);
#endif
        totals.c_completion_states += atomic_load_explicit(
            &workers[i].stats.c_completion_states,
            memory_order_relaxed);
#endif
#endif
        totals.bound_prunes += atomic_load_explicit(
            &workers[i].stats.bound_prunes, memory_order_relaxed);
        totals.linear_place_prunes += atomic_load_explicit(
            &workers[i].stats.linear_place_prunes, memory_order_relaxed);
        totals.pair_bound_prunes += atomic_load_explicit(
            &workers[i].stats.pair_bound_prunes, memory_order_relaxed);
        totals.prefix_place_prunes += atomic_load_explicit(
            &workers[i].stats.prefix_place_prunes, memory_order_relaxed);
#if ADDITIVE_PERM_BOUNDS_ACTIVE
        totals.additive_perm_prunes += atomic_load_explicit(
            &workers[i].stats.additive_perm_prunes, memory_order_relaxed);
#if ADDITIVE_PERM_LINEAR_ACTIVE
        totals.linear_perm_prunes += atomic_load_explicit(
            &workers[i].stats.linear_perm_prunes, memory_order_relaxed);
#endif
#endif
        totals.all_subset_place_prunes += atomic_load_explicit(
            &workers[i].stats.all_subset_place_prunes,
            memory_order_relaxed);
        totals.all_subset_place_fair_count += atomic_load_explicit(
            &workers[i].stats.all_subset_place_fair_count,
            memory_order_relaxed);
        totals.permutation_fair_count += atomic_load_explicit(
            &workers[i].stats.permutation_fair_count, memory_order_relaxed);
    }
    return totals;
}

static void run_prefix(struct worker *worker, uint64_t prefix)
{
    struct search *search = &worker->search;
    uint64_t choices = prefix;
    unsigned selected[SEARCH_COLUMNS];
    unsigned column;
    unsigned row = worker->shared->start_row;

    if (row + 1U >= DICE) {
        accept_configuration(search);
        return;
    }
    for (column = 0; column < worker->shared->prefix_columns; ++column) {
        unsigned mask = search->candidate_mask[row][column];
        unsigned radix = (unsigned)__builtin_popcount(mask);
        unsigned choice = (unsigned)(choices % radix);
        unsigned candidate;

        choices /= radix;
        while (choice != 0U) {
            mask &= mask - 1U;
            --choice;
        }
        candidate = (unsigned)__builtin_ctz(mask);
        selected[column] = candidate;
        apply_choice(search, row, column, candidate);
    }

    search_row(search, row, worker->shared->prefix_columns);

    column = worker->shared->prefix_columns;
    while (column-- > 0) {
        undo_choice(search, row, column, selected[column]);
    }
}

static bool run_job(struct worker *worker, uint64_t job)
{
    const struct shared_state *shared = worker->shared;
    uint64_t prefix = job;

    for (;;) {
        if (atomic_load_explicit(&shared->stop, memory_order_relaxed)) {
            return false;
        }
        run_prefix(worker, prefix);
        if (atomic_load_explicit(&shared->stop, memory_order_relaxed)) {
            return false;
        }
        if (shared->job_count >= shared->prefix_count - prefix) {
            return true;
        }
        prefix += shared->job_count;
    }
}

static void *worker_main(void *argument)
{
    struct worker *worker = argument;
    struct shared_state *shared = worker->shared;

    while (!atomic_load_explicit(&shared->stop, memory_order_relaxed)) {
        uint64_t ticket = atomic_fetch_add_explicit(
            &shared->next_job, 1, memory_order_relaxed);
        uint64_t job;

        if (ticket >= shared->job_count || sigint_requested) {
            if (sigint_requested) {
                atomic_store_explicit(&shared->stop, true,
                                      memory_order_relaxed);
            }
            break;
        }
        job = shared->job_order == NULL
            ? ticket
            : shared->job_order[ticket];
        if (run_job(worker, job)) {
            atomic_fetch_add_explicit(&shared->jobs_done, 1,
                                      memory_order_relaxed);
        }
        publish_worker_stats(&worker->search);
    }

    publish_worker_stats(&worker->search);
    pthread_mutex_lock(&shared->completion_mutex);
    --shared->workers_running;
    pthread_cond_signal(&shared->completion_condition);
    pthread_mutex_unlock(&shared->completion_mutex);
    return NULL;
}

static bool append_solution_batch(const struct shared_state *shared,
                                  const struct solution *solutions,
                                  unsigned count)
{
    char *records = shared->solution_write_buffer;
    size_t length = 0;
    size_t written = 0;
    int descriptor = -1;
    int saved_errno;
    unsigned i;

    for (i = 0; i < count; ++i) {
        const struct solution *solution = &solutions[i];
        size_t remaining = SOLUTION_RECORD_CAPACITY;
        int result = snprintf(
            records + length, remaining,
            "%s config=%dd%d mirror=%d mirror-columns=%d encoding=%s\n",
            solution_kind_name(solution->kind), DICE, SIDES,
            FULL_MIRROR, MIRROR_COLUMNS, solution->encoding);

        if (result < 0 || (size_t)result >= remaining) {
            errno = EOVERFLOW;
            return false;
        }
        length += (size_t)result;
    }

    descriptor = open(shared->options.solutions_path,
                      O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (descriptor < 0) {
        return false;
    }
    while (flock(descriptor, LOCK_EX) != 0) {
        if (errno != EINTR) {
            goto fail;
        }
    }
    while (written < length) {
        ssize_t result = write(descriptor, records + written,
                               length - written);

        if (result > 0) {
            written += (size_t)result;
        } else if (result == 0) {
            errno = EIO;
            goto fail;
        } else if (errno != EINTR) {
            goto fail;
        }
    }
    /* Closing releases the lock; no descriptor is retained between batches. */
    if (close(descriptor) != 0) {
        return false;
    }
    return true;

fail:
    saved_errno = errno;
    close(descriptor);
    errno = saved_errno;
    return false;
}

static void drain_solutions(struct shared_state *shared)
{
    struct solution *batch;
    unsigned count;
    unsigned i;

    pthread_mutex_lock(&shared->solution_mutex);
    count = shared->solution_count;
    batch = shared->solution_queue;
    if (count != 0) {
        shared->solution_queue = shared->solution_batch;
        shared->solution_batch = batch;
        shared->solution_count = 0;
        pthread_cond_broadcast(&shared->solution_not_full);
    }
    pthread_mutex_unlock(&shared->solution_mutex);

    if (count == 0) {
        return;
    }
#if !FULL_MIRROR
    for (i = 0; i < count; ++i) {
        const struct solution *solution = &batch[i];
        unsigned face;
        bool symmetric = solution->kind == PERMUTATION_FAIR;

        for (face = 0; symmetric && face < FACE_COUNT / 2U; ++face) {
            symmetric = solution->encoding[face] ==
                solution->encoding[FACE_COUNT - face - 1U];
        }
        shared->mirror_symmetric_total += symmetric;
    }
#endif
    if (shared->options.solutions_path != NULL &&
        !shared->solution_file_failed &&
        !append_solution_batch(shared, batch, count)) {
        int append_errno = errno;

        fprintf(stderr, "Unable to append solutions to '%s': %s\n",
                shared->options.solutions_path,
                strerror(append_errno));
        shared->solution_file_failed = true;
        atomic_store_explicit(&shared->internal_error, true,
                              memory_order_relaxed);
        atomic_store_explicit(&shared->stop, true,
                              memory_order_relaxed);
    }
    for (i = 0; i < count; ++i) {
        const struct solution *solution = &batch[i];

        if (solution->number <= shared->options.print_limit) {
            printf("%s #%" PRIu64 " encoding=%s\n",
                   solution_kind_name(solution->kind),
                   solution->number, solution->encoding);
        }
    }
    fflush(stdout);
}

static void print_progress(const struct shared_state *shared,
                           const struct worker *workers, double start_time,
                           unsigned workers_running)
{
    struct totals totals = collect_totals(workers, shared->thread_count);
    uint64_t jobs_done = atomic_load_explicit(&shared->jobs_done,
                                              memory_order_relaxed);
    uint64_t permutation_total = atomic_load_explicit(
        &shared->permutation_total, memory_order_relaxed);
    uint64_t mirror_symmetric_total = FULL_MIRROR
        ? permutation_total
        : shared->mirror_symmetric_total;
    char nodes[SI_COUNT_TEXT_SIZE];
#if ROW2_MITM_ACTIVE
    char mitm_solves[SI_COUNT_TEXT_SIZE];
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    char completion_checks[SI_COUNT_TEXT_SIZE];
    char completion_prunes[SI_COUNT_TEXT_SIZE];
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    char early_completion_checks[SI_COUNT_TEXT_SIZE];
    char early_completion_prunes[SI_COUNT_TEXT_SIZE];
    char early_completion_states[SI_COUNT_TEXT_SIZE];
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
    char c_completion_checks[SI_COUNT_TEXT_SIZE];
    char c_completion_prunes[SI_COUNT_TEXT_SIZE];
#if INCREMENTAL_C_COMPLETION_COUPLED
    char c_completion_coupled_prunes[SI_COUNT_TEXT_SIZE];
#endif
    char c_completion_states[SI_COUNT_TEXT_SIZE];
#endif
#endif
    char bound_prunes[SI_COUNT_TEXT_SIZE];
    char linear_place_prunes[SI_COUNT_TEXT_SIZE];
    char pair_bound_prunes[SI_COUNT_TEXT_SIZE];
    char permutation_fair[SI_COUNT_TEXT_SIZE];
    char mirror_symmetric[SI_COUNT_TEXT_SIZE];
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    char additive_perm_prunes[SI_COUNT_TEXT_SIZE];
#if ADDITIVE_PERM_LINEAR_ACTIVE
    char linear_perm_prunes[SI_COUNT_TEXT_SIZE];
#endif
#elif !PERM_ONLY
    uint64_t all_subset_total = atomic_load_explicit(
        &shared->all_subset_total, memory_order_relaxed);
    char prefix_place_prunes[SI_COUNT_TEXT_SIZE];
    char all_subset_place_prunes[SI_COUNT_TEXT_SIZE];
    char all_subset_place_fair[SI_COUNT_TEXT_SIZE];
#endif

    format_si_count(nodes, totals.nodes);
#if ROW2_MITM_ACTIVE
    format_si_count(mitm_solves, totals.mitm_solves);
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    format_si_count(completion_checks, totals.completion_checks);
    format_si_count(completion_prunes, totals.completion_prunes);
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
    format_si_count(early_completion_checks,
                    totals.early_completion_checks);
    format_si_count(early_completion_prunes,
                    totals.early_completion_prunes);
    format_si_count(early_completion_states,
                    totals.early_completion_states);
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
    format_si_count(c_completion_checks, totals.c_completion_checks);
    format_si_count(c_completion_prunes, totals.c_completion_prunes);
#if INCREMENTAL_C_COMPLETION_COUPLED
    format_si_count(c_completion_coupled_prunes,
                    totals.c_completion_coupled_prunes);
#endif
    format_si_count(c_completion_states, totals.c_completion_states);
#endif
#endif
    format_si_count(bound_prunes, totals.bound_prunes);
    format_si_count(linear_place_prunes, totals.linear_place_prunes);
    format_si_count(pair_bound_prunes, totals.pair_bound_prunes);
    format_si_count(permutation_fair, permutation_total);
    format_si_count(mirror_symmetric, mirror_symmetric_total);
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    format_si_count(additive_perm_prunes, totals.additive_perm_prunes);
#if ADDITIVE_PERM_LINEAR_ACTIVE
    format_si_count(linear_perm_prunes, totals.linear_perm_prunes);
#endif
#elif !PERM_ONLY
    format_si_count(prefix_place_prunes, totals.prefix_place_prunes);
    format_si_count(all_subset_place_prunes,
                    totals.all_subset_place_prunes);
    format_si_count(all_subset_place_fair, all_subset_total);
#endif

    fprintf(stderr,
            "progress: %.1fs workers=%u jobs=%" PRIu64 "/%" PRIu64
            " nodes=%s"
            " place-pruned=%s"
            " linear-place-pruned=%s"
#if !ROW1_MITM_ACTIVE
            " pair-pruned=%s"
#if ADDITIVE_PERM_BOUNDS_ACTIVE
            " additive-perm-pruned=%s"
#if ADDITIVE_PERM_LINEAR_ACTIVE
            " linear-perm-pruned=%s"
#endif
#elif !PERM_ONLY
            " prefix-place-pruned=%s"
            " all-subset-place-pruned=%s"
            " all-subset-place-fair=%s"
#endif
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
            " early-completion-checks=%s early-completion-pruned=%s"
            " early-completion-states=%s"
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
            " c-completion-checks=%s c-completion-pruned=%s"
#if INCREMENTAL_C_COMPLETION_COUPLED
            " c-coupled-pruned=%s"
#endif
            " c-completion-states=%s"
#endif
            " completion-checks=%s completion-pruned=%s"
#endif
#if ROW2_MITM_ACTIVE
            " mitm-solves=%s"
#endif
            " permutation-fair=%s mirror-symmetric=%s\n",
            monotonic_seconds() - start_time, workers_running, jobs_done,
            shared->job_count, nodes,
            bound_prunes,
            linear_place_prunes,
#if !ROW1_MITM_ACTIVE
            pair_bound_prunes,
#if ADDITIVE_PERM_BOUNDS_ACTIVE
            additive_perm_prunes,
#if ADDITIVE_PERM_LINEAR_ACTIVE
            linear_perm_prunes,
#endif
#elif !PERM_ONLY
            prefix_place_prunes,
            all_subset_place_prunes,
            all_subset_place_fair,
#endif
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
            early_completion_checks, early_completion_prunes,
            early_completion_states,
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
            c_completion_checks, c_completion_prunes,
#if INCREMENTAL_C_COMPLETION_COUPLED
            c_completion_coupled_prunes,
#endif
            c_completion_states,
#endif
            completion_checks, completion_prunes,
#endif
#if ROW2_MITM_ACTIVE
            mitm_solves,
#endif
            permutation_fair, mirror_symmetric);
    fflush(stderr);
}

static bool solution_batch_ready(struct shared_state *shared)
{
    bool ready;

    pthread_mutex_lock(&shared->solution_mutex);
    ready = shared->solution_count >= SOLUTION_FLUSH_THRESHOLD;
    pthread_mutex_unlock(&shared->solution_mutex);
    return ready;
}

static void watch_workers(struct shared_state *shared,
                          const struct worker *workers, double start_time)
{
    bool suppression_reported = false;
    double last_progress = start_time;

    for (;;) {
        unsigned running;
        int wait_result = 0;

        pthread_mutex_lock(&shared->completion_mutex);
        running = shared->workers_running;
        if (running != 0 && !sigint_requested &&
            !solution_batch_ready(shared)) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            ++deadline.tv_sec;
            wait_result = pthread_cond_timedwait(
                &shared->completion_condition, &shared->completion_mutex,
                &deadline);
            running = shared->workers_running;
        }
        pthread_mutex_unlock(&shared->completion_mutex);

        if (sigint_requested) {
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
        }
        if (wait_result != 0 && wait_result != ETIMEDOUT &&
            !sigint_requested) {
            atomic_store_explicit(&shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
        }

        drain_solutions(shared);
        if (!suppression_reported && shared->options.print_limit != 0 &&
            shared->options.print_limit != UINT64_MAX &&
            (atomic_load_explicit(&shared->all_subset_total,
                                  memory_order_relaxed) >
                 shared->options.print_limit ||
             atomic_load_explicit(&shared->permutation_total,
                                  memory_order_relaxed) >
                 shared->options.print_limit)) {
            fprintf(stderr,
                    "Terminal solution output limited to the first %" PRIu64
#if PERM_ONLY
                    " results; use --all-solutions for the complete stream",
#else
                    " results in each class; use --all-solutions for the "
                    "complete streams",
#endif
                    shared->options.print_limit);
            if (shared->options.solutions_path != NULL) {
                fputs("; the solutions file still receives every result",
                      stderr);
            }
            fputs(".\n", stderr);
            suppression_reported = true;
        }
        if (running == 0) {
            break;
        }
        if (shared->options.progress_seconds != 0 &&
            monotonic_seconds() - last_progress >=
                (double)shared->options.progress_seconds) {
            print_progress(shared, workers, start_time, running);
            last_progress = monotonic_seconds();
        }
    }
}

static void print_template_plan(FILE *stream, const struct search *search,
                                unsigned start_row, const char *path)
{
    unsigned row;

    fprintf(stream, "Template plan: file=%s, row-order=", path);
    for (row = 0; row < DICE; ++row) {
        fprintf(stream, "%s%c(%u)", row == 0 ? "" : ",",
                'A' + search->logical_die[row],
                search->fixed_row_count[row]);
    }
    fprintf(stream, ", prebuilt-rows=%u", start_row);
    if (start_row + 1U < DICE) {
        unsigned column;

        fprintf(stream, ", next-row=%c, column-order=",
                'A' + search->logical_die[start_row]);
        for (column = 0; column < SEARCH_COLUMNS; ++column) {
            fprintf(stream, "%s%u", column == 0 ? "" : ",",
                    search->column_order[start_row][column]);
        }
    }
    fputc('\n', stream);
}

int main(int argc, char **argv)
{
    struct shared_state shared;
    struct worker *workers = NULL;
    struct search *prototype = NULL;
    struct options options;
    unsigned requested_threads;
    unsigned created_threads = 0;
    unsigned i;
    int exit_status = EXIT_SUCCESS;
    double start_time;

    if (!parse_options(argc, argv, &options)) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (!install_sigint_handler()) {
        return EXIT_FAILURE;
    }
    if (options.random_order && !options.seed_given) {
        options.seed = default_random_seed();
    }

    requested_threads = options.threads;
    if (requested_threads == 0) {
        long online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        requested_threads = online_cpus > 0 ? (unsigned)online_cpus : 1U;
        if (requested_threads > MAX_THREADS) {
            requested_threads = MAX_THREADS;
        }
    }

    memset(&shared, 0, sizeof(shared));
    shared.options = options;
    prototype = malloc(sizeof(*prototype));
    if (prototype == NULL) {
        fprintf(stderr, "Unable to allocate initial search state.\n");
        return EXIT_FAILURE;
    }
    if (!initialize_search(prototype, &options, &shared.start_row)) {
        free(prototype);
        return EXIT_FAILURE;
    }
    shared.prefix_count = 1;
    {
        uint64_t desired_jobs = options.jobs != 0
            ? options.jobs
            : (uint64_t)requested_threads * JOBS_PER_WORKER;

        while (prototype->template_possible &&
               shared.start_row + 1U < DICE &&
               shared.prefix_columns < SEARCH_COLUMNS) {
            unsigned mask = prototype->candidate_mask[shared.start_row]
                [shared.prefix_columns];
            unsigned radix = (unsigned)__builtin_popcount(mask);

            if (radix > 1U && shared.prefix_count >= desired_jobs) {
                break;
            }
            if (radix == 0 || shared.prefix_count > UINT64_MAX / radix) {
                fprintf(stderr,
                        "Requested job count requires too many search prefixes.\n");
                free(prototype);
                return EXIT_FAILURE;
            }
            shared.prefix_count *= radix;
            ++shared.prefix_columns;
            if (radix > 1U) {
                ++shared.split_depth;
            }
        }
        shared.job_count = options.jobs != 0 &&
                           options.jobs < shared.prefix_count
            ? options.jobs
            : shared.prefix_count;
        if (options.jobs != 0 && shared.job_count != options.jobs) {
            fprintf(stderr,
                    "Requested %" PRIu64 " jobs, but this search supports at "
                    "most %" PRIu64 " prefix jobs; using that maximum.\n",
                    options.jobs, shared.job_count);
        }
    }
    shared.thread_count = requested_threads;
    if (shared.thread_count > shared.job_count) {
        shared.thread_count = (unsigned)shared.job_count;
    }

    shared.solution_queue = malloc(
        (size_t)SOLUTION_QUEUE_CAPACITY * sizeof(*shared.solution_queue));
    shared.solution_batch = malloc(
        (size_t)SOLUTION_QUEUE_CAPACITY * sizeof(*shared.solution_batch));
    if (options.solutions_path != NULL) {
        shared.solution_write_buffer = malloc(
            (size_t)SOLUTION_QUEUE_CAPACITY * SOLUTION_RECORD_CAPACITY);
    }
    if (shared.solution_queue == NULL || shared.solution_batch == NULL ||
        (options.solutions_path != NULL &&
         shared.solution_write_buffer == NULL)) {
        fprintf(stderr, "Unable to allocate solution output buffers.\n");
        free(shared.solution_write_buffer);
        free(shared.solution_batch);
        free(shared.solution_queue);
        free(prototype);
        return EXIT_FAILURE;
    }

    atomic_init(&shared.next_job, 0);
    atomic_init(&shared.jobs_done, 0);
    atomic_init(&shared.limit_claims, 0);
    atomic_init(&shared.all_subset_total, 0);
    atomic_init(&shared.permutation_total, 0);
    atomic_init(&shared.stop, false);
    atomic_init(&shared.internal_error, false);
    if (pthread_mutex_init(&shared.completion_mutex, NULL) != 0 ||
        pthread_cond_init(&shared.completion_condition, NULL) != 0 ||
        pthread_mutex_init(&shared.solution_mutex, NULL) != 0 ||
        pthread_cond_init(&shared.solution_not_full, NULL) != 0) {
        fprintf(stderr, "Unable to initialize pthread synchronization.\n");
        free(shared.solution_write_buffer);
        free(shared.solution_batch);
        free(shared.solution_queue);
        free(prototype);
        return EXIT_FAILURE;
    }
    if (!initialize_job_order(&shared)) {
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    workers = calloc(shared.thread_count, sizeof(*workers));
    if (workers == NULL) {
        fprintf(stderr, "Unable to allocate worker state.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }
    workers[0].search = *prototype;
    free(prototype);
    prototype = NULL;
    for (i = 0; i < shared.thread_count; ++i) {
        if (i != 0) {
            workers[i].search = workers[0].search;
        }
        workers[i].id = i;
        workers[i].shared = &shared;
        workers[i].search.shared = &shared;
        workers[i].search.published = &workers[i].stats;
        atomic_init(&workers[i].stats.nodes, 0);
#if ROW2_MITM_ACTIVE
        atomic_init(&workers[i].stats.mitm_solves, 0);
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
        atomic_init(&workers[i].stats.completion_checks, 0);
        atomic_init(&workers[i].stats.completion_prunes, 0);
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
        atomic_init(&workers[i].stats.early_completion_checks, 0);
        atomic_init(&workers[i].stats.early_completion_prunes, 0);
        atomic_init(&workers[i].stats.early_completion_states, 0);
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
        atomic_init(&workers[i].stats.c_completion_checks, 0);
        atomic_init(&workers[i].stats.c_completion_prunes, 0);
#if INCREMENTAL_C_COMPLETION_COUPLED
        atomic_init(&workers[i].stats.c_completion_coupled_prunes, 0);
#endif
        atomic_init(&workers[i].stats.c_completion_states, 0);
#endif
#endif
        atomic_init(&workers[i].stats.bound_prunes, 0);
        atomic_init(&workers[i].stats.linear_place_prunes, 0);
        atomic_init(&workers[i].stats.pair_bound_prunes, 0);
        atomic_init(&workers[i].stats.prefix_place_prunes, 0);
#if ADDITIVE_PERM_BOUNDS_ACTIVE
        atomic_init(&workers[i].stats.additive_perm_prunes, 0);
#if ADDITIVE_PERM_LINEAR_ACTIVE
        atomic_init(&workers[i].stats.linear_perm_prunes, 0);
#endif
#endif
        atomic_init(&workers[i].stats.all_subset_place_prunes, 0);
        atomic_init(&workers[i].stats.all_subset_place_fair_count, 0);
        atomic_init(&workers[i].stats.permutation_fair_count, 0);
    }

    if (options.template_active) {
        print_template_plan(stderr, &workers[0].search, shared.start_row,
                            options.template_path);
    }
    fprintf(stderr,
            "Searching %s%s%dd%d column-grouped configurations (%s) "
            "with %u pthread workers (%" PRIu64 " jobs over %" PRIu64
            " prefixes, split depth %u; "
            "place goal %" PRIu64 "; mirror-columns=%u; job order=%s",
            PERM_ONLY ? "permutation-only " : "",
            FULL_MIRROR ? "mirrored " :
                (MIRROR_COLUMNS > 0 ? "partially mirrored " : ""),
            DICE, SIDES,
            traversal_description(),
            shared.thread_count,
            shared.job_count, shared.prefix_count, shared.split_depth,
            workers[0].search.place_goal,
            (unsigned)MIRROR_COLUMNS,
            options.random_order ? "random" : "sequential");
#if INCREMENTAL_C_COMPLETION_ACTIVE
    fprintf(stderr, ", c-completion-directions=%u",
            (unsigned)C_COMPLETION_ACTIVE_BASE_DIRECTION_COUNT);
#if INCREMENTAL_C_COMPLETION_COUPLED
    fputs("+X+Y/X-Y", stderr);
#endif
#endif
    if (options.random_order) {
        fprintf(stderr, ", seed=%" PRIu64, options.seed);
    }
    if (options.solutions_path != NULL) {
        fprintf(stderr, ", solutions-file=%s", options.solutions_path);
    }
    fputs(")\n", stderr);

    if (!workers[0].search.template_possible) {
        fprintf(stderr,
                "No solution is possible because a fully fixed template row "
                "fails a required fairness check.\n");
    } else if (workers[0].search.outcome_count % DICE != 0) {
        fprintf(stderr,
                "No place-fair configuration is possible because %d^%d "
                "is not divisible by %d.\n",
                SIDES, DICE, DICE);
    } else if ((SIDES % 2U) != 0) {
        fprintf(stderr,
                "No all-subset-place-fair configuration is possible because "
                "%d^2 is not divisible by 2.\n",
                SIDES);
#if PERM_ONLY
    } else if (!workers[0].search.permutation_fairness_possible) {
        fprintf(stderr,
                "No permutation-fair configuration is possible because a "
                "required outcome count is not divisible by its factorial.\n");
#endif
    } else {
        start_time = monotonic_seconds();
        for (i = 0; i < shared.thread_count; ++i) {
            int result;

            pthread_mutex_lock(&shared.completion_mutex);
            ++shared.workers_running;
            pthread_mutex_unlock(&shared.completion_mutex);
            result = pthread_create(&workers[i].thread, NULL, worker_main,
                                    &workers[i]);
            if (result != 0) {
                fprintf(stderr, "Unable to create worker %u: %s\n", i,
                        strerror(result));
                pthread_mutex_lock(&shared.completion_mutex);
                --shared.workers_running;
                pthread_mutex_unlock(&shared.completion_mutex);
                atomic_store_explicit(&shared.stop, true,
                                      memory_order_relaxed);
                exit_status = EXIT_FAILURE;
                break;
            }
            ++created_threads;
        }

        if (created_threads != 0) {
            watch_workers(&shared, workers, start_time);
        }
        for (i = 0; i < created_threads; ++i) {
            int result = pthread_join(workers[i].thread, NULL);
            if (result != 0) {
                fprintf(stderr, "Unable to join worker %u: %s\n", i,
                        strerror(result));
                exit_status = EXIT_FAILURE;
            }
        }
        drain_solutions(&shared);

        {
            struct totals totals = collect_totals(workers,
                                                  shared.thread_count);
            double elapsed = monotonic_seconds() - start_time;
            double nodes_per_second = elapsed > 0.0
                ? (double)totals.nodes / elapsed
                : 0.0;
            uint64_t rounded_nodes_per_second =
                nodes_per_second >= (double)UINT64_MAX
                    ? UINT64_MAX
                    : (uint64_t)(nodes_per_second + 0.5);
            uint64_t permutation_total = atomic_load_explicit(
                &shared.permutation_total, memory_order_relaxed);
            uint64_t mirror_symmetric_total = FULL_MIRROR
                ? permutation_total
                : shared.mirror_symmetric_total;
            uint64_t jobs_done = atomic_load_explicit(
                &shared.jobs_done, memory_order_relaxed);
            bool search_error = atomic_load_explicit(
                &shared.internal_error, memory_order_relaxed);
            bool hit_limit = options.limit != 0 &&
                atomic_load_explicit(&shared.limit_claims,
                                     memory_order_relaxed) >= options.limit;
            bool incomplete_search = jobs_done != shared.job_count &&
                !hit_limit && !sigint_requested;
            bool search_failed;
            char nodes[SI_COUNT_TEXT_SIZE];
#if ROW2_MITM_ACTIVE
            char mitm_solves[SI_COUNT_TEXT_SIZE];
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
            char completion_checks[SI_COUNT_TEXT_SIZE];
            char completion_prunes[SI_COUNT_TEXT_SIZE];
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
            char early_completion_checks[SI_COUNT_TEXT_SIZE];
            char early_completion_prunes[SI_COUNT_TEXT_SIZE];
            char early_completion_states[SI_COUNT_TEXT_SIZE];
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
            char c_completion_checks[SI_COUNT_TEXT_SIZE];
            char c_completion_prunes[SI_COUNT_TEXT_SIZE];
#if INCREMENTAL_C_COMPLETION_COUPLED
            char c_completion_coupled_prunes[SI_COUNT_TEXT_SIZE];
#endif
            char c_completion_states[SI_COUNT_TEXT_SIZE];
#endif
#endif
            char node_rate[SI_COUNT_TEXT_SIZE];
            char bound_prunes[SI_COUNT_TEXT_SIZE];
            char linear_place_prunes[SI_COUNT_TEXT_SIZE];
            char pair_bound_prunes[SI_COUNT_TEXT_SIZE];
            char permutation_fair[SI_COUNT_TEXT_SIZE];
            char mirror_symmetric[SI_COUNT_TEXT_SIZE];
#if ADDITIVE_PERM_BOUNDS_ACTIVE
            char additive_perm_prunes[SI_COUNT_TEXT_SIZE];
#if ADDITIVE_PERM_LINEAR_ACTIVE
            char linear_perm_prunes[SI_COUNT_TEXT_SIZE];
#endif
#elif !PERM_ONLY
            uint64_t all_subset_total = atomic_load_explicit(
                &shared.all_subset_total, memory_order_relaxed);
            char prefix_place_prunes[SI_COUNT_TEXT_SIZE];
            char all_subset_place_prunes[SI_COUNT_TEXT_SIZE];
            char all_subset_place_fair[SI_COUNT_TEXT_SIZE];
#endif

            format_si_count(nodes, totals.nodes);
#if ROW2_MITM_ACTIVE
            format_si_count(mitm_solves, totals.mitm_solves);
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
            format_si_count(completion_checks, totals.completion_checks);
            format_si_count(completion_prunes, totals.completion_prunes);
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
            format_si_count(early_completion_checks,
                            totals.early_completion_checks);
            format_si_count(early_completion_prunes,
                            totals.early_completion_prunes);
            format_si_count(early_completion_states,
                            totals.early_completion_states);
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
            format_si_count(c_completion_checks,
                            totals.c_completion_checks);
            format_si_count(c_completion_prunes,
                            totals.c_completion_prunes);
#if INCREMENTAL_C_COMPLETION_COUPLED
            format_si_count(c_completion_coupled_prunes,
                            totals.c_completion_coupled_prunes);
#endif
            format_si_count(c_completion_states,
                            totals.c_completion_states);
#endif
#endif
            format_si_count(node_rate, rounded_nodes_per_second);
            format_si_count(bound_prunes, totals.bound_prunes);
            format_si_count(linear_place_prunes,
                            totals.linear_place_prunes);
            format_si_count(pair_bound_prunes, totals.pair_bound_prunes);
            format_si_count(permutation_fair, permutation_total);
            format_si_count(mirror_symmetric, mirror_symmetric_total);
#if ADDITIVE_PERM_BOUNDS_ACTIVE
            format_si_count(additive_perm_prunes,
                            totals.additive_perm_prunes);
#if ADDITIVE_PERM_LINEAR_ACTIVE
            format_si_count(linear_perm_prunes,
                            totals.linear_perm_prunes);
#endif
#elif !PERM_ONLY
            format_si_count(prefix_place_prunes,
                            totals.prefix_place_prunes);
            format_si_count(all_subset_place_prunes,
                            totals.all_subset_place_prunes);
            format_si_count(all_subset_place_fair, all_subset_total);
#endif

            if (search_error) {
                exit_status = EXIT_FAILURE;
            }
            if (incomplete_search) {
                if (exit_status == EXIT_SUCCESS) {
                    fprintf(stderr,
                            "Search stopped before every scheduled job "
                            "completed.\n");
                }
                exit_status = EXIT_FAILURE;
            }
            search_failed = exit_status != EXIT_SUCCESS;

            if (sigint_requested) {
                fprintf(stderr,
                        "Interrupted search configuration: %s%s%dd%d "
                        "column-grouped, traversal=%s, workers=%u, jobs=%"
                        PRIu64 " over %" PRIu64 " prefixes, split-depth=%u, "
                        "mirror-columns=%u, job-order=%s",
                        PERM_ONLY ? "permutation-only " : "",
                        FULL_MIRROR ? "mirrored " :
                            (MIRROR_COLUMNS > 0 ?
                                "partially mirrored " : ""),
                        DICE, SIDES,
                        traversal_description(), shared.thread_count,
                        shared.job_count, shared.prefix_count,
                        shared.split_depth,
                        (unsigned)MIRROR_COLUMNS,
                        options.random_order ? "random" : "sequential");
#if INCREMENTAL_C_COMPLETION_ACTIVE
                fprintf(stderr, ", c-completion-directions=%u",
                        (unsigned)C_COMPLETION_ACTIVE_BASE_DIRECTION_COUNT);
#if INCREMENTAL_C_COMPLETION_COUPLED
                fputs("+X+Y/X-Y", stderr);
#endif
#endif
                if (options.random_order) {
                    fprintf(stderr, ", seed=%" PRIu64, options.seed);
                }
                if (options.solutions_path != NULL) {
                    fprintf(stderr, ", solutions-file=%s",
                            options.solutions_path);
                }
                if (options.template_active) {
                    unsigned row;

                    fprintf(stderr, ", template=%s, row-order=",
                            options.template_path);
                    for (row = 0; row < DICE; ++row) {
                        fprintf(stderr, "%s%c", row == 0 ? "" : ",",
                                'A' + workers[0].search.logical_die[row]);
                    }
                }
                fputc('\n', stderr);
            }

            fprintf(stderr,
                    "Search %s: %.2fs, %u workers, jobs=%" PRIu64 "/%" PRIu64
                    ", nodes=%s (%s/s)"
                    ", place-pruned=%s"
                    ", linear-place-prunes=%s"
#if !ROW1_MITM_ACTIVE
                    ", pair-pruned=%s"
#if ADDITIVE_PERM_BOUNDS_ACTIVE
                    ", additive-perm-prunes=%s"
#if ADDITIVE_PERM_LINEAR_ACTIVE
                    ", linear-perm-prunes=%s"
#endif
#elif !PERM_ONLY
                    ", prefix-place-prunes=%s"
                    ", all-subset-place-prunes=%s"
                    ", all-subset-place-fair=%s"
#endif
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
                    ", early-completion-checks=%s"
                    ", early-completion-pruned=%s"
                    ", early-completion-states=%s"
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
                    ", c-completion-checks=%s"
                    ", c-completion-pruned=%s"
#if INCREMENTAL_C_COMPLETION_COUPLED
                    ", c-coupled-pruned=%s"
#endif
                    ", c-completion-states=%s"
#endif
                    ", completion-checks=%s, completion-pruned=%s"
#endif
#if ROW2_MITM_ACTIVE
                    ", mitm-solves=%s"
#endif
                    ", permutation-fair=%s, mirror-symmetric=%s\n",
                    sigint_requested ? "interrupted" :
                        (search_failed ? "failed" :
                            (hit_limit ? "stopped at limit" : "complete")),
                    elapsed,
                    shared.thread_count,
                    jobs_done,
                    shared.job_count, nodes, node_rate,
                    bound_prunes, linear_place_prunes,
#if !ROW1_MITM_ACTIVE
                    pair_bound_prunes,
#if ADDITIVE_PERM_BOUNDS_ACTIVE
                    additive_perm_prunes,
#if ADDITIVE_PERM_LINEAR_ACTIVE
                    linear_perm_prunes,
#endif
#elif !PERM_ONLY
                    prefix_place_prunes,
                    all_subset_place_prunes,
                    all_subset_place_fair,
#endif
#endif
#if CONDITIONED_COMPLETION_BOUNDS_ACTIVE
#if EARLY_CONDITIONED_COMPLETION_BOUNDS_ACTIVE
                    early_completion_checks, early_completion_prunes,
                    early_completion_states,
#endif
#if INCREMENTAL_C_COMPLETION_ACTIVE
                    c_completion_checks, c_completion_prunes,
#if INCREMENTAL_C_COMPLETION_COUPLED
                    c_completion_coupled_prunes,
#endif
                    c_completion_states,
#endif
                    completion_checks, completion_prunes,
#endif
#if ROW2_MITM_ACTIVE
                    mitm_solves,
#endif
                    permutation_fair, mirror_symmetric);
        }
    }

    if (atomic_load_explicit(&shared.internal_error, memory_order_relaxed)) {
        if (!shared.solution_file_failed) {
            fprintf(stderr, "A worker reported an internal search error.\n");
        }
        exit_status = EXIT_FAILURE;
    }
    if (sigint_requested && exit_status == EXIT_SUCCESS) {
        exit_status = 128 + SIGINT;
    }

cleanup:
    drain_solutions(&shared);
    free(prototype);
    free(workers);
    free(shared.job_order);
    free(shared.solution_write_buffer);
    free(shared.solution_batch);
    free(shared.solution_queue);
    pthread_cond_destroy(&shared.solution_not_full);
    pthread_mutex_destroy(&shared.solution_mutex);
    pthread_cond_destroy(&shared.completion_condition);
    pthread_mutex_destroy(&shared.completion_mutex);
    return exit_status;
}
