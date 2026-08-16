#define _POSIX_C_SOURCE 200809L

/*
 * Exhaustive search for column-grouped go-first dice.
 *
 * DICE and SIDES deliberately are compile-time constants.  Override the
 * defaults with, for example:
 *
 *   cc -std=c11 -O3 -march=native -flto \
 *      -DDICE=4 -DSIDES=18 -DMIRROR=1 \
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

#if MIRROR != 0 && MIRROR != 1
#error "MIRROR must be either 0 or 1"
#endif

#if MIRROR && ((SIDES % 2) != 0)
#error "Mirrored column-grouped dice require an even SIDES value"
#endif

#define FACE_COUNT (DICE * SIDES)
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

/* Number of permutations and partial permutations, including the empty one. */
#if DICE == 2
#define DICE_FACTORIAL UINT64_C(2)
#define PERM_STATE_COUNT 5
#elif DICE == 3
#define DICE_FACTORIAL UINT64_C(6)
#define PERM_STATE_COUNT 16
#elif DICE == 4
#define DICE_FACTORIAL UINT64_C(24)
#define PERM_STATE_COUNT 65
#elif DICE == 5
#define DICE_FACTORIAL UINT64_C(120)
#define PERM_STATE_COUNT 326
#elif DICE == 6
#define DICE_FACTORIAL UINT64_C(720)
#define PERM_STATE_COUNT 1957
#else
#error "column_search supports DICE values from 2 through 6"
#endif

_Static_assert(SIDES > 0, "SIDES must be positive");

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

struct perm_counter {
    struct perm_state state[PERM_STATE_COUNT];
    int transition[PERM_STATE_COUNT][DICE];
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

    uint64_t outcome_count;
    uint64_t place_goal;
    uint64_t permutation_goal;
    bool permutation_fairness_possible;

    struct perm_counter permutations;
    struct shared_state *shared;
    struct worker_stats *published;

    uint64_t nodes;
    uint64_t bound_prunes;
    uint64_t pair_prunes;
    uint64_t place_fair_count;
    uint64_t permutation_fair_count;
};

struct worker_stats {
    atomic_uint_fast64_t nodes;
    atomic_uint_fast64_t bound_prunes;
    atomic_uint_fast64_t pair_prunes;
    atomic_uint_fast64_t place_fair_count;
    atomic_uint_fast64_t permutation_fair_count;
};

struct solution {
    struct solution *next;
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
    atomic_uint_fast64_t permutation_total;
    atomic_bool stop;
    atomic_bool internal_error;

    pthread_mutex_t completion_mutex;
    pthread_cond_t completion_condition;
    unsigned workers_running;

    pthread_mutex_t solution_mutex;
    struct solution *solution_head;
    struct solution *solution_tail;
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
            "The first column is fixed to remove equivalent die renamings.\n"
            "\n"
            "  -t, --threads N     worker threads; default is online CPUs\n"
            "  -n, --limit N       stop after N place-and-pair-fair candidates\n"
            "  -p, --progress N    progress interval in seconds; 0 disables\n"
            "      --print-limit N print at most N permutation-fair solutions\n"
            "      --all-solutions print every permutation-fair solution\n"
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

    for (next = 0; next < PERM_STATE_COUNT; ++next) {
        const struct perm_state *state = &counter->state[next];
        unsigned die;

        for (die = 0; die < DICE; ++die) {
            if ((state->mask & (1U << die)) != 0 || state->length == DICE) {
                counter->transition[next][die] = -1;
            } else {
                uint64_t key = state->key |
                    ((uint64_t)(die + 1U) << (4U * state->length));
                counter->transition[next][die] =
                    find_perm_state(counter, state->length + 1U, key);
                if (counter->transition[next][die] < 0) {
                    return false;
                }
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

/* Count roll orderings by scanning labels from low to high. */
static bool check_permutation_fairness(struct search *search)
{
    struct perm_counter *counter = &search->permutations;
    unsigned face;

    memset(counter->ways, 0, sizeof(counter->ways));
    counter->ways[0] = 1; /* one way to choose an empty ordering */

    for (face = 0; face < FACE_COUNT; ++face) {
        unsigned owner = search->owner[face];
        size_t i = PERM_STATE_COUNT;

        /* Destinations have greater indices, so reverse traversal is in-place. */
        while (i-- > 0) {
            int destination = counter->transition[i][owner];
            if (destination >= 0) {
                counter->ways[destination] += counter->ways[i];
            }
        }
    }

    if (!search->permutation_fairness_possible) {
        return false;
    }
    {
        size_t i;
        for (i = counter->length_begin[DICE];
             i < counter->length_begin[DICE + 1]; ++i) {
            if (counter->ways[i] != search->permutation_goal) {
                return false;
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

static uint64_t choice_contribution(const struct search *search,
                                    unsigned candidate, unsigned column,
                                    unsigned place)
{
    unsigned face = search->grid[candidate][column];
    uint64_t contribution = search->contribution[face][place];

#if MIRROR
    unsigned mirror_column = SIDES - column - 1U;
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

            for (candidate = row; candidate < DICE; ++candidate) {
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
    atomic_store_explicit(&search->published->pair_prunes,
                          search->pair_prunes, memory_order_relaxed);
    atomic_store_explicit(&search->published->place_fair_count,
                          search->place_fair_count, memory_order_relaxed);
    atomic_store_explicit(&search->published->permutation_fair_count,
                          search->permutation_fair_count,
                          memory_order_relaxed);
}

static void record_solution(struct search *search)
{
    struct shared_state *shared = search->shared;
    struct solution *solution = NULL;
    uint64_t number;
    unsigned face;

    pthread_mutex_lock(&shared->solution_mutex);
    number = atomic_fetch_add_explicit(&shared->permutation_total, 1,
                                       memory_order_relaxed) + 1U;
    if (number <= shared->options.print_limit) {
        solution = malloc(sizeof(*solution));
        if (solution != NULL) {
            solution->next = NULL;
            solution->number = number;
            for (face = 0; face < FACE_COUNT; ++face) {
                solution->encoding[face] =
                    (char)('A' + search->owner[face]);
            }
            solution->encoding[FACE_COUNT] = '\0';
            if (shared->solution_tail == NULL) {
                shared->solution_head = solution;
            } else {
                shared->solution_tail->next = solution;
            }
            shared->solution_tail = solution;
        }
    }
    pthread_mutex_unlock(&shared->solution_mutex);

    if (number <= shared->options.print_limit && solution == NULL) {
        atomic_store_explicit(&shared->internal_error, true,
                              memory_order_relaxed);
        atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
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

    if (shared->options.limit != 0) {
        limit_claim = atomic_fetch_add_explicit(&shared->limit_claims, 1,
                                                memory_order_relaxed);
        if (limit_claim >= shared->options.limit) {
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
            return;
        }
    }

    ++search->place_fair_count;
    build_owner_table(search);
    permutation_fair = check_permutation_fairness(search);
    if (permutation_fair) {
        ++search->permutation_fair_count;
        record_solution(search);
    }

    if (shared->options.limit != 0 &&
        limit_claim + 1U >= shared->options.limit) {
        atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
    }
}

static void apply_choice(struct search *search, unsigned row,
                         unsigned column, unsigned candidate)
{
    unsigned temporary = search->grid[row][column];
    unsigned chosen_face;
    unsigned place;

    search->grid[row][column] = search->grid[candidate][column];
    search->grid[candidate][column] = temporary;
    chosen_face = search->grid[row][column];

#if MIRROR
    {
        unsigned mirror_column = SIDES - column - 1U;
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
    unsigned chosen_face = search->grid[row][column];
    unsigned place;
    unsigned temporary;

    for (place = 0; place < DICE; ++place) {
        search->tally[row][place] -= search->contribution[chosen_face][place];
#if MIRROR
        search->tally[row][place] -= search->contribution[
            search->grid[row][SIDES - column - 1U]][place];
#endif
    }

    temporary = search->grid[row][column];
    search->grid[row][column] = search->grid[candidate][column];
    search->grid[candidate][column] = temporary;
#if MIRROR
    {
        unsigned mirror_column = SIDES - column - 1U;
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
    if (!bounds_allow_goal(search, row, column)) {
        ++search->bound_prunes;
        return;
    }

    if (column == SEARCH_COLUMNS) {
        unsigned place;
        for (place = 0; place < DICE; ++place) {
            if (search->tally[row][place] != search->place_goal) {
                return;
            }
        }
        if (!die_is_pairwise_fair(search, row)) {
            ++search->pair_prunes;
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
    if (search->permutation_fairness_possible) {
        search->permutation_goal = search->outcome_count / DICE_FACTORIAL;
    }

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
    uint64_t pair_prunes;
    uint64_t place_fair_count;
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
        totals.pair_prunes += atomic_load_explicit(
            &workers[i].stats.pair_prunes, memory_order_relaxed);
        totals.place_fair_count += atomic_load_explicit(
            &workers[i].stats.place_fair_count, memory_order_relaxed);
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
    struct solution *solution;

    pthread_mutex_lock(&shared->solution_mutex);
    solution = shared->solution_head;
    shared->solution_head = NULL;
    shared->solution_tail = NULL;
    pthread_mutex_unlock(&shared->solution_mutex);

    while (solution != NULL) {
        struct solution *next = solution->next;
        printf("permutation-fair #%" PRIu64 " encoding=%s\n",
               solution->number, solution->encoding);
        free(solution);
        solution = next;
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
            " pair-pruned=%" PRIu64 " place+pair-fair=%" PRIu64
            " permutation-fair=%" PRIu64 "\n",
            monotonic_seconds() - start_time, workers_running, jobs_done,
            shared->job_count, totals.nodes, totals.bound_prunes, totals.pair_prunes,
            totals.place_fair_count,
            atomic_load_explicit(&shared->permutation_total,
                                 memory_order_relaxed));
    fflush(stderr);
}

static void watch_workers(struct shared_state *shared,
                          const struct worker *workers, double start_time)
{
    bool suppression_reported = false;

    for (;;) {
        unsigned running;
        int wait_result = 0;

        pthread_mutex_lock(&shared->completion_mutex);
        running = shared->workers_running;
        if (running != 0) {
            if (shared->options.progress_seconds == 0) {
                wait_result = pthread_cond_wait(&shared->completion_condition,
                                                &shared->completion_mutex);
            } else {
                struct timespec deadline;
                clock_gettime(CLOCK_REALTIME, &deadline);
                deadline.tv_sec +=
                    (time_t)shared->options.progress_seconds;
                wait_result = pthread_cond_timedwait(
                    &shared->completion_condition, &shared->completion_mutex,
                    &deadline);
            }
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
            atomic_load_explicit(&shared->permutation_total,
                                 memory_order_relaxed) >
                shared->options.print_limit) {
            fprintf(stderr,
                    "Solution output suppressed after %" PRIu64
                    " results; use --all-solutions for the complete stream.\n",
                    shared->options.print_limit);
            suppression_reported = true;
        }
        if (running == 0) {
            break;
        }
        if (wait_result == ETIMEDOUT &&
            shared->options.progress_seconds != 0) {
            print_progress(shared, workers, start_time, running);
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
    atomic_init(&shared.permutation_total, 0);
    atomic_init(&shared.stop, false);
    atomic_init(&shared.internal_error, false);
    if (pthread_mutex_init(&shared.completion_mutex, NULL) != 0 ||
        pthread_cond_init(&shared.completion_condition, NULL) != 0 ||
        pthread_mutex_init(&shared.solution_mutex, NULL) != 0) {
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
        atomic_init(&workers[i].stats.pair_prunes, 0);
        atomic_init(&workers[i].stats.place_fair_count, 0);
        atomic_init(&workers[i].stats.permutation_fair_count, 0);
    }

    fprintf(stderr,
            "Searching %s%dd%d column-grouped configurations "
            "with %u pthread workers (%" PRIu64 " jobs, split depth %u; "
            "place goal %" PRIu64 ")\n",
            MIRROR ? "mirrored " : "", DICE, SIDES, shared.thread_count,
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
                    ", pair-prunes=%" PRIu64 ", place+pair-fair=%" PRIu64
                    ", permutation-fair=%" PRIu64 "\n",
                    hit_limit ? "stopped at limit" : "complete", elapsed,
                    shared.thread_count,
                    atomic_load_explicit(&shared.jobs_done,
                                         memory_order_relaxed),
                    shared.job_count, totals.nodes, nodes_per_second,
                    totals.bound_prunes, totals.pair_prunes,
                    totals.place_fair_count,
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
    pthread_mutex_destroy(&shared.solution_mutex);
    pthread_cond_destroy(&shared.completion_condition);
    pthread_mutex_destroy(&shared.completion_mutex);
    return exit_status;
}
