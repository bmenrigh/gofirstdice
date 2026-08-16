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
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#if MIRROR != 0 && MIRROR != 1
#error "MIRROR must be either 0 or 1"
#endif

#if HIGH_FIRST != 0 && HIGH_FIRST != 1
#error "HIGH_FIRST must be either 0 or 1"
#endif

#if MIRROR && ((SIDES % 2) != 0)
#error "Mirrored column-grouped dice require an even SIDES value"
#endif

#if !MIRROR && SIDES <= 255
#define PACKED_PAIR_WINS 1
#else
#define PACKED_PAIR_WINS 0
#endif

#define FACE_COUNT (DICE * SIDES)
#define PLACE_DIRECTION_COUNT ((DICE * (DICE - 1)) / 2)
#if MIRROR
#define SEARCH_COLUMNS (SIDES / 2)
#else
#define SEARCH_COLUMNS SIDES
#endif
#define DEFAULT_PRINT_LIMIT UINT64_C(10)
#define DEFAULT_PROGRESS_SECONDS UINT64_C(1)
#define PROGRESS_CHECK_MASK UINT64_C(0x3ffff)
#define JOBS_PER_WORKER UINT64_C(8)
#define MAX_THREADS 256U
#define SOLUTION_QUEUE_CAPACITY 256U
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

/* Number of permutations and partial permutations, including the empty one. */
#if DICE == 2
#define DICE_FACTORIAL UINT64_C(2)
#define PERM_STATE_COUNT 5
#define PERM_EDGE_COUNT 2
#elif DICE == 3
#define DICE_FACTORIAL UINT64_C(6)
#define PERM_STATE_COUNT 16
#define PERM_EDGE_COUNT 5
#elif DICE == 4
#define DICE_FACTORIAL UINT64_C(24)
#define PERM_STATE_COUNT 65
#define PERM_EDGE_COUNT 16
#elif DICE == 5
#define DICE_FACTORIAL UINT64_C(120)
#define PERM_STATE_COUNT 326
#define PERM_EDGE_COUNT 65
#elif DICE == 6
#define DICE_FACTORIAL UINT64_C(720)
#define PERM_STATE_COUNT 1957
#define PERM_EDGE_COUNT 326
#else
#error "column_search supports DICE values from 2 through 6"
#endif

_Static_assert(SIDES > 0, "SIDES must be positive");
_Static_assert(PAIR_BOUND_START <= SEARCH_COLUMNS,
               "PAIR_BOUND_START exceeds the searched columns");
_Static_assert(LINEAR_BOUND_START <= SEARCH_COLUMNS,
               "LINEAR_BOUND_START exceeds the searched columns");
_Static_assert(LINEAR_BOUND_STRIDE > 0,
               "LINEAR_BOUND_STRIDE must be positive");
_Static_assert(PERM_STATE_COUNT <= UINT16_MAX,
               "permutation state indices must fit in uint16_t");

struct options {
    uint64_t limit;
    uint64_t print_limit;
    uint64_t progress_seconds;
    unsigned threads;
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

struct shared_state;
struct worker_stats;

struct search {
    /* Face labels are zero based internally. */
    unsigned grid[DICE][SIDES];
    unsigned owner[FACE_COUNT];

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
#if PACKED_PAIR_WINS
    /* One byte-wide win counter per previous die, packed into a word. */
    uint64_t pair_wins[DICE];
    uint64_t pair_increment[DICE][SEARCH_COLUMNS][DICE];
    uint64_t pair_minimum_left[DICE][SEARCH_COLUMNS + 1];
    uint64_t pair_maximum_left[DICE][SEARCH_COLUMNS + 1];
#elif !MIRROR
    unsigned pair_wins[DICE][DICE];
#endif

    uint64_t outcome_count;
    uint64_t place_goal;
    bool permutation_fairness_possible;
    bool linear_place_bounds_possible;

    struct perm_counter permutations;
    struct shared_state *shared;
    struct worker_stats *published;

    uint64_t nodes;
    uint64_t bound_prunes;
    uint64_t linear_place_prunes;
    uint64_t pair_bound_prunes;
    uint64_t pair_prunes;
    uint64_t prefix_place_prunes;
    uint64_t all_subset_place_prunes;
    uint64_t all_subset_place_fair_count;
    uint64_t permutation_fair_count;
};

struct worker_stats {
    atomic_uint_fast64_t nodes;
    atomic_uint_fast64_t bound_prunes;
    atomic_uint_fast64_t linear_place_prunes;
    atomic_uint_fast64_t pair_bound_prunes;
    atomic_uint_fast64_t pair_prunes;
    atomic_uint_fast64_t prefix_place_prunes;
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
    unsigned split_depth;
    uint64_t job_count;

    atomic_uint_fast64_t next_job;
    atomic_uint_fast64_t jobs_done;
    atomic_uint_fast64_t limit_claims;
    atomic_uint_fast64_t all_subset_total;
    atomic_uint_fast64_t permutation_total;
    atomic_bool stop;
    atomic_bool internal_error;

    pthread_mutex_t completion_mutex;
    pthread_cond_t completion_condition;
    unsigned workers_running;

    pthread_mutex_t solution_mutex;
    pthread_cond_t solution_not_full;
    struct solution solution_queue[SOLUTION_QUEUE_CAPACITY];
    unsigned solution_head;
    unsigned solution_tail;
    unsigned solution_count;
};

struct worker {
    pthread_t thread;
    unsigned id;
    struct shared_state *shared;
    struct worker_stats stats;
    struct search search;
};

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [OPTIONS]\n"
            "\n"
            "Search the compile-time-selected %s%dd%d column-grouped space.\n"
            "The first searched column is fixed to remove equivalent die "
            "renamings.\n"
            "Results are place-fair for every subset. Fully permutation-fair "
            "results are reported only in the stronger class.\n"
            "\n"
            "  -t, --threads N     worker threads; default is online CPUs\n"
            "  -n, --limit N       stop after N reported results\n"
            "  -p, --progress N    progress interval in seconds; 0 disables\n"
            "      --print-limit N print at most N of each solution class\n"
            "      --all-solutions print every solution in both classes\n"
            "  -q, --quiet         print only startup and final counts\n"
            "  -h, --help          show this help\n",
            program, MIRROR ? "mirrored " : "", DICE, SIDES);
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

static bool parse_options(int argc, char **argv, struct options *options)
{
    int i;

    *options = (struct options){
        .print_limit = DEFAULT_PRINT_LIMIT,
        .progress_seconds = DEFAULT_PROGRESS_SECONDS,
    };
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

static void initialize_grid(struct search *search)
{
    unsigned column;

    /* Alternating direction affects traversal order, not the search space. */
#if MIRROR
    for (column = 0; column < SEARCH_COLUMNS; ++column) {
        unsigned die;
        unsigned mirror_column = SIDES - column - 1U;

        for (die = 0; die < DICE; ++die) {
            unsigned offset = (column & 1U) == 0 ? die : DICE - die - 1U;
            unsigned face = column * DICE + offset;

            search->grid[die][column] = face;
            search->grid[die][mirror_column] = FACE_COUNT - face - 1U;
        }
    }
#else
    for (column = 0; column < SIDES; ++column) {
        unsigned die;
        for (die = 0; die < DICE; ++die) {
            unsigned offset = (column & 1U) == 0 ? die : DICE - die - 1U;
            search->grid[die][column] = column * DICE + offset;
        }
    }
#endif
}

/* Translate recursion depth to the physical face-label column. */
static unsigned physical_column(unsigned search_column)
{
#if MIRROR || !HIGH_FIRST
    return search_column;
#else
    return SIDES - search_column - 1U;
#endif
}

#if !MIRROR
static bool pair_tracking_active(unsigned column)
{
#if PAIR_BOUND_START == 0
    (void)column;
    return true;
#else
    return column >= PAIR_BOUND_START;
#endif
}
#endif

static uint64_t choice_contribution(const struct search *search,
                                    unsigned candidate, unsigned column,
                                    unsigned place)
{
    unsigned actual_column = physical_column(column);
    unsigned face = search->grid[candidate][actual_column];
    uint64_t contribution = search->contribution[face][place];

#if MIRROR
    unsigned mirror_column = SIDES - actual_column - 1U;
    unsigned mirror_face = search->grid[candidate][mirror_column];
    contribution += search->contribution[mirror_face][place];
#endif
    return contribution;
}

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
            unsigned candidate_limit = column == 0 ? row + 1U : DICE;

            for (candidate = row; candidate < candidate_limit; ++candidate) {
                uint64_t value = choice_contribution(
                    search, candidate, (unsigned)column, place);
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
                    int64_t minimum = INT64_MAX;
                    int64_t maximum = INT64_MIN;
                    unsigned candidate_limit =
                        column == 0 ? row + 1U : DICE;
                    unsigned candidate;

                    for (candidate = row; candidate < candidate_limit;
                         ++candidate) {
                        int64_t value = (int64_t)choice_contribution(
                            search, candidate, (unsigned)column, first) -
                            (int64_t)choice_contribution(
                                search, candidate, (unsigned)column, second);

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
            unsigned actual_column = physical_column((unsigned)column);
            unsigned candidate_limit = column == 0 ? row + 1U : DICE;
            uint64_t any_wins = 0;
            uint64_t all_win = lane_ones;
            unsigned candidate;

            for (candidate = row; candidate < candidate_limit; ++candidate) {
                uint64_t increment = 0;

                for (previous = 0; previous < row; ++previous) {
                    if (search->grid[candidate][actual_column] >
                        search->grid[previous][actual_column]) {
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
#if MIRROR
    (void)search;
    (void)row;
    (void)column;
    return true;
#else
    unsigned previous;
    unsigned goal = SIDES / 2U;
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

#if !MIRROR
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
            unsigned actual_column = physical_column(column);
            if (search->grid[row][actual_column] >
                search->grid[previous][actual_column]) {
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
 * Off-diagonal column matchups balance automatically: each die wins once for
 * every ordered pair of unequal columns.  The pair is fair exactly when this
 * die wins half of the SIDES same-column matchups.
 */
static bool die_is_pairwise_fair(const struct search *search, unsigned die)
{
#if MIRROR
    /* Complementary faces on the same die make every pair fair. */
    (void)search;
    (void)die;
    return true;
#else
    unsigned previous;

    for (previous = 0; previous < die; ++previous) {
        unsigned wins = 0;
        unsigned column;

        for (column = 0; column < SIDES; ++column) {
            if (search->grid[die][column] > search->grid[previous][column]) {
                ++wins;
            }
        }
        if (wins * 2U != SIDES) {
            return false;
        }
    }
    return true;
#endif
}

/*
 * Every completed prefix is one of the subsets that must be place-fair.
 * Count its ranks directly by scanning labels from low to high.
 */
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
    atomic_store_explicit(&search->published->bound_prunes,
                          search->bound_prunes, memory_order_relaxed);
    atomic_store_explicit(&search->published->linear_place_prunes,
                          search->linear_place_prunes,
                          memory_order_relaxed);
    atomic_store_explicit(&search->published->pair_bound_prunes,
                          search->pair_bound_prunes,
                          memory_order_relaxed);
    atomic_store_explicit(&search->published->pair_prunes,
                          search->pair_prunes, memory_order_relaxed);
    atomic_store_explicit(&search->published->prefix_place_prunes,
                          search->prefix_place_prunes,
                          memory_order_relaxed);
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

static void record_solution(struct search *search, enum solution_kind kind)
{
    struct shared_state *shared = search->shared;
    atomic_uint_fast64_t *total = kind == ALL_SUBSET_PLACE_FAIR
        ? &shared->all_subset_total
        : &shared->permutation_total;
    uint64_t number;
    unsigned face;
    bool enqueued = false;

    pthread_mutex_lock(&shared->solution_mutex);
    number = atomic_fetch_add_explicit(total, 1,
                                       memory_order_relaxed) + 1U;
    if (number <= shared->options.print_limit) {
        struct solution *solution;

        while (shared->solution_count == SOLUTION_QUEUE_CAPACITY) {
            pthread_cond_wait(&shared->solution_not_full,
                              &shared->solution_mutex);
        }
        solution = &shared->solution_queue[shared->solution_tail];
        solution->kind = kind;
        solution->number = number;
        for (face = 0; face < FACE_COUNT; ++face) {
            solution->encoding[face] =
                (char)('A' + search->owner[face]);
        }
        solution->encoding[FACE_COUNT] = '\0';
        shared->solution_tail =
            (shared->solution_tail + 1U) % SOLUTION_QUEUE_CAPACITY;
        ++shared->solution_count;
        enqueued = true;
    }
    pthread_mutex_unlock(&shared->solution_mutex);

    /* Wake the watcher so redirected all-solution output drains promptly. */
    if (enqueued) {
        pthread_cond_signal(&shared->completion_condition);
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
    if (!counted_all_subsets_are_place_fair(search)) {
        ++search->all_subset_place_prunes;
        return;
    }

    permutation_fair = search->permutation_fairness_possible &&
        counted_subset_is_permutation_fair(
            search, (1U << DICE) - 1U, DICE);

    if (shared->options.limit != 0) {
        limit_claim = atomic_fetch_add_explicit(&shared->limit_claims, 1,
                                                memory_order_relaxed);
        if (limit_claim >= shared->options.limit) {
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
            return;
        }
    }

    if (permutation_fair) {
        ++search->permutation_fair_count;
        record_solution(search, PERMUTATION_FAIR);
    } else {
        ++search->all_subset_place_fair_count;
        record_solution(search, ALL_SUBSET_PLACE_FAIR);
    }

    if (shared->options.limit != 0 &&
        limit_claim + 1U >= shared->options.limit) {
        atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
    }
}

static void apply_choice(struct search *search, unsigned row,
                         unsigned column, unsigned candidate)
{
    unsigned actual_column = physical_column(column);
    unsigned temporary = search->grid[row][actual_column];
    unsigned chosen_face;
    unsigned place;

    search->grid[row][actual_column] = search->grid[candidate][actual_column];
    search->grid[candidate][actual_column] = temporary;
    chosen_face = search->grid[row][actual_column];

#if !MIRROR
    if (pair_tracking_active(column)) {
#if PACKED_PAIR_WINS
        search->pair_wins[row] +=
            search->pair_increment[row][column][candidate];
#else
        unsigned previous;

        for (previous = 0; previous < row; ++previous) {
            if (chosen_face > search->grid[previous][actual_column]) {
                ++search->pair_wins[row][previous];
            }
        }
#endif
    }
#endif

#if MIRROR
    {
        unsigned mirror_column = SIDES - actual_column - 1U;
        temporary = search->grid[row][mirror_column];
        search->grid[row][mirror_column] =
            search->grid[candidate][mirror_column];
        search->grid[candidate][mirror_column] = temporary;
    }
#endif

    for (place = 0; place < DICE; ++place) {
        search->tally[row][place] += search->contribution[chosen_face][place];
#if MIRROR
        search->tally[row][place] += search->contribution[
            search->grid[row][SIDES - column - 1U]][place];
#endif
    }
}

static void undo_choice(struct search *search, unsigned row,
                        unsigned column, unsigned candidate)
{
    unsigned actual_column = physical_column(column);
    unsigned chosen_face = search->grid[row][actual_column];
    unsigned place;
    unsigned temporary;

#if !MIRROR
    if (pair_tracking_active(column)) {
#if PACKED_PAIR_WINS
        search->pair_wins[row] -=
            search->pair_increment[row][column][candidate];
#else
        unsigned previous;

        for (previous = 0; previous < row; ++previous) {
            if (chosen_face > search->grid[previous][actual_column]) {
                --search->pair_wins[row][previous];
            }
        }
#endif
    }
#endif

    for (place = 0; place < DICE; ++place) {
        search->tally[row][place] -= search->contribution[chosen_face][place];
#if MIRROR
        search->tally[row][place] -= search->contribution[
            search->grid[row][SIDES - column - 1U]][place];
#endif
    }

    temporary = search->grid[row][actual_column];
    search->grid[row][actual_column] = search->grid[candidate][actual_column];
    search->grid[candidate][actual_column] = temporary;
#if MIRROR
    {
        unsigned mirror_column = SIDES - actual_column - 1U;
        temporary = search->grid[row][mirror_column];
        search->grid[row][mirror_column] =
            search->grid[candidate][mirror_column];
        search->grid[candidate][mirror_column] = temporary;
    }
#endif
}

/*
 * Fill one die from left to right.  Each branch swaps one face into place,
 * adds its contribution, recurses, then subtracts and swaps back.  In mirror
 * mode the complementary face and column are swapped and tallied with it.
 * There are no configuration or tally copies anywhere in the recursion.
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
        build_bounds(search, row);
    }
#if !MIRROR
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

    if (column == SEARCH_COLUMNS) {
        unsigned place;
        for (place = 0; place < DICE; ++place) {
            if (search->tally[row][place] != search->place_goal) {
                return;
            }
        }
#if !MIRROR && ((SIDES % 2) != 0)
        /* Odd sides cannot split the same-column wins exactly in half. */
        if (!die_is_pairwise_fair(search, row)) {
            ++search->pair_prunes;
            return;
        }
#endif
        if (row + 1U >= 3U) {
            build_owner_table(search);
        }
        if (!completed_prefix_is_place_fair(search, row + 1U)) {
            ++search->prefix_place_prunes;
            return;
        }
        if (row + 1U < DICE - 1U) {
            search_row(search, row + 1U, 0);
        } else {
            /* The last die is forced rather than visited by search_row(). */
            if (!die_is_pairwise_fair(search, DICE - 1U)) {
                ++search->pair_prunes;
                return;
            }
            accept_configuration(search);
        }
        return;
    }

    /* Fixing column zero removes the DICE! equivalent die renamings. */
    for (candidate = row; candidate < DICE; ++candidate) {
        apply_choice(search, row, column, candidate);
        search_row(search, row, column + 1U);
        undo_choice(search, row, column, candidate);

        if (column == 0 ||
            atomic_load_explicit(&search->shared->stop,
                                 memory_order_relaxed)) {
            break;
        }
    }
}

static bool initialize_search(struct search *search)
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

    initialize_grid(search);
    if (!build_contributions(search)) {
        fprintf(stderr, "Place contributions overflowed a 64-bit tally.\n");
        return false;
    }
    if (!initialize_perm_counter(&search->permutations)) {
        fprintf(stderr, "Unable to initialize the permutation counter.\n");
        return false;
    }
    return true;
}

struct totals {
    uint64_t nodes;
    uint64_t bound_prunes;
    uint64_t linear_place_prunes;
    uint64_t pair_bound_prunes;
    uint64_t pair_prunes;
    uint64_t prefix_place_prunes;
    uint64_t all_subset_place_prunes;
    uint64_t all_subset_place_fair_count;
    uint64_t permutation_fair_count;
};

static struct totals collect_totals(const struct worker *workers,
                                    unsigned thread_count)
{
    struct totals totals = {0};
    unsigned i;

    for (i = 0; i < thread_count; ++i) {
        totals.nodes += atomic_load_explicit(&workers[i].stats.nodes,
                                             memory_order_relaxed);
        totals.bound_prunes += atomic_load_explicit(
            &workers[i].stats.bound_prunes, memory_order_relaxed);
        totals.linear_place_prunes += atomic_load_explicit(
            &workers[i].stats.linear_place_prunes, memory_order_relaxed);
        totals.pair_bound_prunes += atomic_load_explicit(
            &workers[i].stats.pair_bound_prunes, memory_order_relaxed);
        totals.pair_prunes += atomic_load_explicit(
            &workers[i].stats.pair_prunes, memory_order_relaxed);
        totals.prefix_place_prunes += atomic_load_explicit(
            &workers[i].stats.prefix_place_prunes, memory_order_relaxed);
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

static void run_job(struct worker *worker, uint64_t job)
{
    struct search *search = &worker->search;
    uint64_t choices = job;
    unsigned depth;

    /* Column zero is the fixed global die-renaming representative. */
    apply_choice(search, 0, 0, 0);
    for (depth = 0; depth < worker->shared->split_depth; ++depth) {
        unsigned candidate = (unsigned)(choices % DICE);
        choices /= DICE;
        apply_choice(search, 0, depth + 1U, candidate);
    }

    build_bounds(search, 0);
    search_row(search, 0, worker->shared->split_depth + 1U);

    depth = worker->shared->split_depth;
    choices = job;
    /* Decode again so choices can be undone in reverse order. */
    {
        unsigned selected[SEARCH_COLUMNS];
        unsigned i;
        for (i = 0; i < depth; ++i) {
            selected[i] = (unsigned)(choices % DICE);
            choices /= DICE;
        }
        while (depth-- > 0) {
            undo_choice(search, 0, depth + 1U, selected[depth]);
        }
    }
    undo_choice(search, 0, 0, 0);
}

static void *worker_main(void *argument)
{
    struct worker *worker = argument;
    struct shared_state *shared = worker->shared;

    while (!atomic_load_explicit(&shared->stop, memory_order_relaxed)) {
        uint64_t job = atomic_fetch_add_explicit(&shared->next_job, 1,
                                                 memory_order_relaxed);
        if (job >= shared->job_count) {
            break;
        }
        run_job(worker, job);
        atomic_fetch_add_explicit(&shared->jobs_done, 1,
                                  memory_order_relaxed);
        publish_worker_stats(&worker->search);
    }

    publish_worker_stats(&worker->search);
    pthread_mutex_lock(&shared->completion_mutex);
    --shared->workers_running;
    pthread_cond_signal(&shared->completion_condition);
    pthread_mutex_unlock(&shared->completion_mutex);
    return NULL;
}

static void drain_solutions(struct shared_state *shared)
{
    for (;;) {
        struct solution solution;
        bool have_solution;

        pthread_mutex_lock(&shared->solution_mutex);
        have_solution = shared->solution_count != 0;
        if (have_solution) {
            solution = shared->solution_queue[shared->solution_head];
            shared->solution_head =
                (shared->solution_head + 1U) % SOLUTION_QUEUE_CAPACITY;
            --shared->solution_count;
            pthread_cond_signal(&shared->solution_not_full);
        }
        pthread_mutex_unlock(&shared->solution_mutex);
        if (!have_solution) {
            break;
        }
        printf("%s #%" PRIu64 " encoding=%s\n",
               solution.kind == ALL_SUBSET_PLACE_FAIR
                   ? "all-subset-place-fair"
                   : "permutation-fair",
               solution.number, solution.encoding);
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

    fprintf(stderr,
            "progress: %.1fs workers=%u jobs=%" PRIu64 "/%" PRIu64
            " nodes=%" PRIu64 " pruned=%" PRIu64
            " linear-place-pruned=%" PRIu64
            " pair-bound-pruned=%" PRIu64 " pair-pruned=%" PRIu64
            " prefix-place-pruned=%" PRIu64
            " all-subset-place-pruned=%" PRIu64
            " all-subset-place-fair=%" PRIu64
            " permutation-fair=%" PRIu64 "\n",
            monotonic_seconds() - start_time, workers_running, jobs_done,
            shared->job_count, totals.nodes, totals.bound_prunes,
            totals.linear_place_prunes, totals.pair_bound_prunes,
            totals.pair_prunes,
            totals.prefix_place_prunes,
            totals.all_subset_place_prunes,
            atomic_load_explicit(&shared->all_subset_total,
                                 memory_order_relaxed),
            atomic_load_explicit(&shared->permutation_total,
                                 memory_order_relaxed));
    fflush(stderr);
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
        if (running != 0) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            ++deadline.tv_sec;
            wait_result = pthread_cond_timedwait(
                &shared->completion_condition, &shared->completion_mutex,
                &deadline);
            running = shared->workers_running;
        }
        pthread_mutex_unlock(&shared->completion_mutex);

        if (wait_result != 0 && wait_result != ETIMEDOUT) {
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
                    "Solution output limited to the first %" PRIu64
                    " results in each class; use --all-solutions for the "
                    "complete streams.\n",
                    shared->options.print_limit);
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

int main(int argc, char **argv)
{
    struct shared_state shared;
    struct worker *workers = NULL;
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
    shared.job_count = 1;
    while (shared.split_depth < SEARCH_COLUMNS - 1U &&
           shared.job_count < (uint64_t)requested_threads * JOBS_PER_WORKER) {
        shared.job_count *= DICE;
        ++shared.split_depth;
    }
    shared.thread_count = requested_threads;
    if (shared.thread_count > shared.job_count) {
        shared.thread_count = (unsigned)shared.job_count;
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
        return EXIT_FAILURE;
    }

    workers = calloc(shared.thread_count, sizeof(*workers));
    if (workers == NULL) {
        fprintf(stderr, "Unable to allocate worker state.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }
    if (!initialize_search(&workers[0].search)) {
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }
    for (i = 0; i < shared.thread_count; ++i) {
        if (i != 0) {
            workers[i].search = workers[0].search;
        }
        workers[i].id = i;
        workers[i].shared = &shared;
        workers[i].search.shared = &shared;
        workers[i].search.published = &workers[i].stats;
        atomic_init(&workers[i].stats.nodes, 0);
        atomic_init(&workers[i].stats.bound_prunes, 0);
        atomic_init(&workers[i].stats.linear_place_prunes, 0);
        atomic_init(&workers[i].stats.pair_bound_prunes, 0);
        atomic_init(&workers[i].stats.pair_prunes, 0);
        atomic_init(&workers[i].stats.prefix_place_prunes, 0);
        atomic_init(&workers[i].stats.all_subset_place_prunes, 0);
        atomic_init(&workers[i].stats.all_subset_place_fair_count, 0);
        atomic_init(&workers[i].stats.permutation_fair_count, 0);
    }

    fprintf(stderr,
            "Searching %s%dd%d column-grouped configurations (%s) "
            "with %u pthread workers (%" PRIu64 " jobs, split depth %u; "
            "place goal %" PRIu64 ")\n",
            MIRROR ? "mirrored " : "", DICE, SIDES,
            MIRROR ? "outer pairs first" :
                (HIGH_FIRST ? "highest labels first" : "lowest labels first"),
            shared.thread_count,
            shared.job_count, shared.split_depth,
            workers[0].search.place_goal);

    if (workers[0].search.outcome_count % DICE != 0) {
        fprintf(stderr,
                "No place-fair configuration is possible because %d^%d "
                "is not divisible by %d.\n",
                SIDES, DICE, DICE);
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
            bool hit_limit = options.limit != 0 &&
                atomic_load_explicit(&shared.limit_claims,
                                     memory_order_relaxed) >= options.limit;

            fprintf(stderr,
                    "Search %s: %.2fs, %u workers, jobs=%" PRIu64 "/%" PRIu64
                    ", nodes=%" PRIu64 " (%.0f/s), bound-prunes=%" PRIu64
                    ", linear-place-prunes=%" PRIu64
                    ", pair-bound-prunes=%" PRIu64
                    ", pair-prunes=%" PRIu64
                    ", prefix-place-prunes=%" PRIu64
                    ", all-subset-place-prunes=%" PRIu64
                    ", all-subset-place-fair=%" PRIu64
                    ", permutation-fair=%" PRIu64 "\n",
                    hit_limit ? "stopped at limit" : "complete", elapsed,
                    shared.thread_count,
                    atomic_load_explicit(&shared.jobs_done,
                                         memory_order_relaxed),
                    shared.job_count, totals.nodes, nodes_per_second,
                    totals.bound_prunes, totals.linear_place_prunes,
                    totals.pair_bound_prunes,
                    totals.pair_prunes,
                    totals.prefix_place_prunes,
                    totals.all_subset_place_prunes,
                    atomic_load_explicit(&shared.all_subset_total,
                                         memory_order_relaxed),
                    atomic_load_explicit(&shared.permutation_total,
                                         memory_order_relaxed));
        }
    }

    if (atomic_load_explicit(&shared.internal_error, memory_order_relaxed)) {
        fprintf(stderr, "A worker reported an internal search error.\n");
        exit_status = EXIT_FAILURE;
    }

cleanup:
    drain_solutions(&shared);
    free(workers);
    pthread_cond_destroy(&shared.solution_not_full);
    pthread_mutex_destroy(&shared.solution_mutex);
    pthread_cond_destroy(&shared.completion_condition);
    pthread_mutex_destroy(&shared.completion_mutex);
    return exit_status;
}
