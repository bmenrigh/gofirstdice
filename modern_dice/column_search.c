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
#include <signal.h>
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

#ifndef PERM_ONLY
#define PERM_ONLY 0
#endif

#ifndef ADDITIVE_PERM_LINEAR
#define ADDITIVE_PERM_LINEAR 1
#endif

#if MIRROR != 0 && MIRROR != 1
#error "MIRROR must be either 0 or 1"
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

#define ADDITIVE_PERM_BOUNDS_ACTIVE (PERM_ONLY && DICE >= 4)
#define ADDITIVE_PERM_LINEAR_ACTIVE \
    (ADDITIVE_PERM_BOUNDS_ACTIVE && ADDITIVE_PERM_LINEAR)

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
_Static_assert(PERM_STATE_COUNT <= UINT16_MAX,
               "permutation state indices must fit in uint16_t");

struct options {
    uint64_t limit;
    uint64_t jobs;
    uint64_t seed;
    uint64_t print_limit;
    uint64_t progress_seconds;
    unsigned threads;
    bool random_order;
    bool seed_given;
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

struct search {
    /* Face labels are zero based internally. */
    unsigned grid[DICE][SIDES];
    unsigned owner[FACE_COUNT];

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
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    struct additive_perm_bounds additive_permutations;
#endif
    struct shared_state *shared;
    struct worker_stats *published;

    uint64_t nodes;
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
    unsigned split_depth;
    uint64_t job_count;
    uint64_t prefix_count;
    uint64_t *job_order;

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
#if MIRROR
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
            "The first searched column is fixed to remove equivalent die "
            "renamings.\n"
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

#if MIRROR
    unsigned mirror_column = SIDES - actual_column - 1U;
    unsigned mirror_face = search->grid[candidate][mirror_column];
    contribution += bounds->contribution[row][mirror_face][permutation];
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

        for (permutation = 0; permutation < permutation_count;
             ++permutation) {
            uint64_t minimum = UINT64_MAX;
            uint64_t maximum = 0;
            unsigned candidate;

            for (candidate = row; candidate < DICE; ++candidate) {
                uint64_t contribution = additive_perm_contribution_at(
                    search, bounds, row, candidate, actual_column,
                    permutation);
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
                unsigned candidate = row;
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

static uint64_t choice_contribution_at(const struct search *search,
                                       unsigned candidate,
                                       unsigned actual_column,
                                       unsigned place)
{
    unsigned face = search->grid[candidate][actual_column];
    uint64_t contribution = search->contribution[face][place];

#if MIRROR
    unsigned mirror_column = SIDES - actual_column - 1U;
    unsigned mirror_face = search->grid[candidate][mirror_column];
    contribution += search->contribution[mirror_face][place];
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
#if !MIRROR
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
        unsigned candidate_limit = column == 0 ? row + 1U : DICE;

        for (place = 0; place < DICE; ++place) {
            uint64_t low = UINT64_MAX;
            uint64_t high = 0;
            unsigned candidate;

            for (candidate = row; candidate < candidate_limit; ++candidate) {
                uint64_t value = choice_contribution_at(
                    search, candidate, actual_column, place);

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
                    unsigned candidate_limit =
                        column == 0 ? row + 1U : DICE;
                    unsigned candidate = row;
                    int64_t low =
                        (int64_t)choice_contribution_at(
                            search, candidate, actual_column, first) -
                        (int64_t)choice_contribution_at(
                            search, candidate, actual_column, second);
                    int64_t high = low;

                    for (++candidate; candidate < candidate_limit;
                         ++candidate) {
                        int64_t value = (int64_t)choice_contribution_at(
                            search, candidate, actual_column, first) -
                            (int64_t)choice_contribution_at(
                                search, candidate, actual_column, second);

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

#if !MIRROR
    {
        unsigned previous;

        for (previous = 0; previous < row; ++previous) {
            for (column = 0; column < SEARCH_COLUMNS; ++column) {
                unsigned actual_column =
                    search->column_order[row][column];
                unsigned candidate_limit =
                    column == 0 ? row + 1U : DICE;
                unsigned low = 1;
                unsigned high = 0;
                unsigned candidate;

                for (candidate = row; candidate < candidate_limit;
                     ++candidate) {
                    unsigned value =
                        search->grid[candidate][actual_column] >
                        search->grid[previous][actual_column];

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

    feasible_choices[0] = 1;
    for (column = 1; column < SEARCH_COLUMNS; ++column) {
        unsigned actual_column = search->column_order[row][column];
        unsigned feasible = 0;
        unsigned candidate;

        for (candidate = row; candidate < DICE; ++candidate) {
            bool allowed = true;

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
#if !MIRROR
            if (allowed) {
                unsigned previous;

                for (previous = 0; previous < row; ++previous) {
                    unsigned value =
                        search->grid[candidate][actual_column] >
                        search->grid[previous][actual_column];
                    unsigned low = pair_total_minimum[previous] -
                        pair_minimum[column][previous];
                    unsigned high = pair_total_maximum[previous] -
                        pair_maximum[column][previous];

                    if (value + low > SIDES / 2U ||
                        value + high < SIDES / 2U) {
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

    for (column = 2; column < SEARCH_COLUMNS; ++column) {
        unsigned selected_column = search->column_order[row][column];
        unsigned selected_choices = feasible_choices[column];
        unsigned position = column;

        while (position > 1U &&
               feasible_choices[position - 1U] > selected_choices) {
            search->column_order[row][position] =
                search->column_order[row][position - 1U];
            feasible_choices[position] = feasible_choices[position - 1U];
            --position;
        }
        search->column_order[row][position] = selected_column;
        feasible_choices[position] = selected_choices;
    }
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
                    search, row, candidate, (unsigned)column, place);
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
                            search, row, candidate, (unsigned)column, first) -
                            (int64_t)choice_contribution(
                                search, row, candidate, (unsigned)column,
                                second);

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
            unsigned actual_column = ordered_column(search, row, column);
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
            search->grid[row][SIDES - actual_column - 1U]][place];
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
            search->grid[row][SIDES - actual_column - 1U]][place];
#endif
    }
#if ADDITIVE_PERM_BOUNDS_ACTIVE
    add_additive_perm_choice(search, row, column, false);
#endif

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
        plan_column_order(search, row);
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

    if (column == SEARCH_COLUMNS) {
        unsigned place;
        for (place = 0; place < DICE; ++place) {
            if (search->tally[row][place] != search->place_goal) {
                return;
            }
        }
#if !PERM_ONLY
        if (row + 1U >= 3U) {
            build_owner_table(search);
        }
        if (!completed_prefix_is_place_fair(search, row + 1U)) {
            ++search->prefix_place_prunes;
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
    plan_column_order(search, 0);
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
    return true;
}

struct totals {
    uint64_t nodes;
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
    choices = prefix;
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
    uint64_t permutation_total = atomic_load_explicit(
        &shared->permutation_total, memory_order_relaxed);
    char nodes[SI_COUNT_TEXT_SIZE];
    char bound_prunes[SI_COUNT_TEXT_SIZE];
    char linear_place_prunes[SI_COUNT_TEXT_SIZE];
    char pair_bound_prunes[SI_COUNT_TEXT_SIZE];
    char permutation_fair[SI_COUNT_TEXT_SIZE];
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
    format_si_count(bound_prunes, totals.bound_prunes);
    format_si_count(linear_place_prunes, totals.linear_place_prunes);
    format_si_count(pair_bound_prunes, totals.pair_bound_prunes);
    format_si_count(permutation_fair, permutation_total);
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
            " nodes=%s pruned=%s"
            " linear-place-pruned=%s"
            " pair-bound-pruned=%s"
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
            " permutation-fair=%s\n",
            monotonic_seconds() - start_time, workers_running, jobs_done,
            shared->job_count, nodes, bound_prunes,
            linear_place_prunes, pair_bound_prunes,
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
            permutation_fair);
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
                    "Solution output limited to the first %" PRIu64
#if PERM_ONLY
                    " results; use --all-solutions for the complete stream.\n",
#else
                    " results in each class; use --all-solutions for the "
                    "complete streams.\n",
#endif
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
    shared.prefix_count = 1;
    {
        uint64_t desired_jobs = options.jobs != 0
            ? options.jobs
            : (uint64_t)requested_threads * JOBS_PER_WORKER;

        while (shared.split_depth < SEARCH_COLUMNS - 1U &&
               shared.prefix_count < desired_jobs) {
            if (shared.prefix_count > UINT64_MAX / DICE) {
                fprintf(stderr,
                        "Requested job count requires too many search prefixes.\n");
                return EXIT_FAILURE;
            }
            shared.prefix_count *= DICE;
            ++shared.split_depth;
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

    fprintf(stderr,
            "Searching %s%s%dd%d column-grouped configurations (%s) "
            "with %u pthread workers (%" PRIu64 " jobs over %" PRIu64
            " prefixes, split depth %u; "
            "place goal %" PRIu64 "; job order=%s",
            PERM_ONLY ? "permutation-only " : "",
            MIRROR ? "mirrored " : "", DICE, SIDES,
            traversal_description(),
            shared.thread_count,
            shared.job_count, shared.prefix_count, shared.split_depth,
            workers[0].search.place_goal,
            options.random_order ? "random" : "sequential");
    if (options.random_order) {
        fprintf(stderr, ", seed=%" PRIu64, options.seed);
    }
    fputs(")\n", stderr);

    if (workers[0].search.outcome_count % DICE != 0) {
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
            bool hit_limit = options.limit != 0 &&
                atomic_load_explicit(&shared.limit_claims,
                                     memory_order_relaxed) >= options.limit;
            char nodes[SI_COUNT_TEXT_SIZE];
            char node_rate[SI_COUNT_TEXT_SIZE];
            char bound_prunes[SI_COUNT_TEXT_SIZE];
            char linear_place_prunes[SI_COUNT_TEXT_SIZE];
            char pair_bound_prunes[SI_COUNT_TEXT_SIZE];
            char permutation_fair[SI_COUNT_TEXT_SIZE];
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
            format_si_count(node_rate, rounded_nodes_per_second);
            format_si_count(bound_prunes, totals.bound_prunes);
            format_si_count(linear_place_prunes,
                            totals.linear_place_prunes);
            format_si_count(pair_bound_prunes, totals.pair_bound_prunes);
            format_si_count(permutation_fair, permutation_total);
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

            if (sigint_requested) {
                fprintf(stderr,
                        "Interrupted search configuration: %s%s%dd%d "
                        "column-grouped, traversal=%s, workers=%u, jobs=%"
                        PRIu64 " over %" PRIu64 " prefixes, split-depth=%u, "
                        "job-order=%s",
                        PERM_ONLY ? "permutation-only " : "",
                        MIRROR ? "mirrored " : "", DICE, SIDES,
                        traversal_description(), shared.thread_count,
                        shared.job_count, shared.prefix_count,
                        shared.split_depth,
                        options.random_order ? "random" : "sequential");
                if (options.random_order) {
                    fprintf(stderr, ", seed=%" PRIu64, options.seed);
                }
                fputc('\n', stderr);
            }

            fprintf(stderr,
                    "Search %s: %.2fs, %u workers, jobs=%" PRIu64 "/%" PRIu64
                    ", nodes=%s (%s/s), bound-prunes=%s"
                    ", linear-place-prunes=%s"
                    ", pair-bound-prunes=%s"
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
                    ", permutation-fair=%s\n",
                    sigint_requested ? "interrupted" :
                        (hit_limit ? "stopped at limit" : "complete"),
                    elapsed,
                    shared.thread_count,
                    atomic_load_explicit(&shared.jobs_done,
                                         memory_order_relaxed),
                    shared.job_count, nodes, node_rate,
                    bound_prunes, linear_place_prunes,
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
                    permutation_fair);
        }
    }

    if (atomic_load_explicit(&shared.internal_error, memory_order_relaxed)) {
        fprintf(stderr, "A worker reported an internal search error.\n");
        exit_status = EXIT_FAILURE;
    }
    if (sigint_requested && exit_status == EXIT_SUCCESS) {
        exit_status = 128 + SIGINT;
    }

cleanup:
    drain_solutions(&shared);
    free(workers);
    free(shared.job_order);
    pthread_cond_destroy(&shared.solution_not_full);
    pthread_mutex_destroy(&shared.solution_mutex);
    pthread_cond_destroy(&shared.completion_condition);
    pthread_mutex_destroy(&shared.completion_mutex);
    return exit_status;
}
