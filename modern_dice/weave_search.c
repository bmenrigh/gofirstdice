#define _POSIX_C_SOURCE 200809L

/*
 * Exhaustive permutation-fair dice search by weaving one die at a time.
 *
 * A configuration is represented by its owner string in increasing label
 * order.  To add die n, choose SIDES nondecreasing insertion gaps in the
 * existing string.  For every ordering of the n + 1 dice, a face inserted at
 * a particular gap contributes
 *
 *   lower-prefix ways * upper-suffix ways.
 *
 * Those contributions are computed once per parent configuration.  The hot
 * recursion only adds/removes table entries and applies remaining-face bounds.
 * Pthreads are confined to a driver above that recursion: logical jobs divide
 * canonical two-die prefixes, and every worker owns an independent search
 * state and all mutable pruning tables.
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

#define FACE_COUNT (DICE * SIDES)
#define DEFAULT_PRINT_LIMIT UINT64_C(10)
#define DEFAULT_PROGRESS_SECONDS UINT64_C(1)
#define PROGRESS_CHECK_MASK UINT64_C(0x3ffff)
#define JOBS_PER_WORKER UINT64_C(8)
#define PREFIXES_PER_JOB UINT64_C(8)
#define MAX_THREADS 256U
#define SOLUTION_QUEUE_CAPACITY 256U

#if DICE == 2
#define MAX_PERMUTATIONS 2U
#define PERM_STATE_COUNT 5U
#define PERM_EDGE_COUNT 2U
#elif DICE == 3
#define MAX_PERMUTATIONS 6U
#define PERM_STATE_COUNT 16U
#define PERM_EDGE_COUNT 5U
#elif DICE == 4
#define MAX_PERMUTATIONS 24U
#define PERM_STATE_COUNT 65U
#define PERM_EDGE_COUNT 16U
#elif DICE == 5
#define MAX_PERMUTATIONS 120U
#define PERM_STATE_COUNT 326U
#define PERM_EDGE_COUNT 65U
#elif DICE == 6
#define MAX_PERMUTATIONS 720U
#define PERM_STATE_COUNT 1957U
#define PERM_EDGE_COUNT 326U
#else
#error "weave_search supports DICE values from 2 through 6"
#endif

#define MAX_OLD_DICE (DICE - 1U)
#if DICE >= 3
#define MAX_TRIPLE_SUBSETS (((DICE - 1U) * (DICE - 2U)) / 2U)
#define MAX_TRIPLE_PROJECTIONS (5U * MAX_TRIPLE_SUBSETS)
#else
/* ISO C does not permit zero-length arrays. */
#define MAX_TRIPLE_SUBSETS 1U
#define MAX_TRIPLE_PROJECTIONS 1U
#endif

/* A fixed new face contributes at most SIDES^2 outcomes to a triple. */
#if SIDES <= 181
typedef int16_t triple_projection_t;
#elif SIDES <= 46340
typedef int32_t triple_projection_t;
#else
typedef int64_t triple_projection_t;
#endif

_Static_assert(SIDES > 0, "SIDES must be positive");
_Static_assert(SIDES <= UINT16_MAX, "insertion gaps must fit in uint16_t");
_Static_assert(FACE_COUNT <= UINT16_MAX, "face count must fit in uint16_t");
_Static_assert(PERM_STATE_COUNT <= UINT16_MAX,
               "permutation state indices must fit in uint16_t");

struct options {
    uint64_t limit;
    uint64_t jobs;
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
    size_t length_begin[DICE + 2U];
};

struct weave_stage {
    unsigned dice;
    unsigned old_length;
    unsigned gap_count;
    unsigned permutation_count;
    uint64_t goal;
    uint64_t place_goal;
    bool possible;
    bool difference_bounds_possible;
    uint16_t prefix_state[MAX_PERMUTATIONS];
    uint16_t reverse_suffix_state[MAX_PERMUTATIONS];
    uint16_t full_state[MAX_PERMUTATIONS];
    uint8_t new_die_position[MAX_PERMUTATIONS];
    uint64_t *contribution;
    uint64_t *minimum_from;
    uint64_t *maximum_from;
    uint64_t *gcd_from;
    uint64_t *place_contribution;
    uint64_t *place_minimum_from;
    uint64_t *place_maximum_from;
    uint64_t *place_gcd_from;
    uint16_t *pair_contribution;
    unsigned triple_projection_count;
    triple_projection_t *triple_difference_contribution;
    triple_projection_t *triple_difference_minimum_from;
    triple_projection_t *triple_difference_maximum_from;
    int64_t *difference_contribution;
    int64_t *difference_minimum_from;
    int64_t *difference_maximum_from;
    uint64_t *difference_gcd_from;
};

struct shared_state;

struct published_stats {
    atomic_uint_fast64_t configurations[DICE + 1U];
    atomic_uint_fast64_t nodes[DICE + 1U];
    atomic_uint_fast64_t label_symmetry_skips[DICE + 1U];
    atomic_uint_fast64_t place_projection_prunes[DICE + 1U];
    atomic_uint_fast64_t pairwise_prunes[DICE + 1U];
    atomic_uint_fast64_t triplet_prunes[DICE + 1U];
    atomic_uint_fast64_t difference_projection_prunes[DICE + 1U];
    atomic_uint_fast64_t bound_prunes[DICE + 1U];
    atomic_uint_fast64_t congruence_prunes[DICE + 1U];
};

struct search {
    struct options options;
    struct shared_state *shared;
    struct published_stats *published;
    struct perm_counter permutations;
    struct weave_stage stage[DICE + 1U];
    uint8_t word[DICE + 1U][FACE_COUNT];
    uint16_t insertion_gap[DICE + 1U][SIDES];
    uint64_t tally[DICE + 1U][MAX_PERMUTATIONS];
    uint64_t place_tally[DICE + 1U][DICE];
    uint64_t pair_tally[DICE + 1U][MAX_OLD_DICE];
    int64_t triple_difference_tally[DICE + 1U]
                                    [MAX_TRIPLE_PROJECTIONS];
    uint64_t configurations[DICE + 1U];
    uint64_t nodes[DICE + 1U];
    uint64_t label_symmetry_skips[DICE + 1U];
    uint64_t place_projection_prunes[DICE + 1U];
    uint64_t pairwise_prunes[DICE + 1U];
    uint64_t triplet_prunes[DICE + 1U];
    uint64_t difference_projection_prunes[DICE + 1U];
    uint64_t bound_prunes[DICE + 1U];
    uint64_t congruence_prunes[DICE + 1U];
    uint64_t total_nodes;
    uint64_t *above;
    uint64_t lower_ways[PERM_STATE_COUNT];
    uint64_t reverse_ways[PERM_STATE_COUNT];
    bool hit_limit;
    bool internal_error;
};

struct queued_solution {
    uint64_t number;
    char encoding[FACE_COUNT + 1U];
};

struct shared_state {
    struct options options;
    uint64_t prefix_count;
    uint64_t job_count;
    unsigned prefix_faces;
    unsigned thread_count;
    atomic_uint_fast64_t next_job;
    atomic_uint_fast64_t jobs_done;
    atomic_uint_fast64_t solution_total;
    atomic_uint active_workers;
    atomic_bool stop;
    atomic_bool internal_error;
    pthread_mutex_t solution_mutex;
    pthread_cond_t solution_not_full;
    struct queued_solution solution_queue[SOLUTION_QUEUE_CAPACITY];
    unsigned solution_head;
    unsigned solution_tail;
    unsigned solution_count;
};

struct worker {
    pthread_t thread;
    struct search *search;
    struct shared_state *shared;
    struct published_stats stats;
    unsigned id;
};

static volatile sig_atomic_t interrupt_requested;

static void handle_sigint(int signal_number)
{
    (void)signal_number;
    interrupt_requested = 1;
}

static bool install_sigint_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_sigint;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0) {
        fprintf(stderr, "Unable to install SIGINT handler: %s\n",
                strerror(errno));
        return false;
    }
    return true;
}

static double monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0.0;
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [OPTIONS]\n\n"
            "Exhaustively build permutation-fair %dd%d dice by weaving one "
            "new die into each fair prefix.\n\n"
            "  -t, --threads N     worker threads; default is online CPUs\n"
            "  -j, --jobs N        logical prefix jobs; default is automatic\n"
            "  -n, --limit N       stop after N final configurations\n"
            "  -p, --progress N    progress interval in seconds; 0 disables\n"
            "      --print-limit N print at most N final configurations\n"
            "      --all-solutions print every final configuration\n"
            "  -q, --quiet         print only startup and final counts\n"
            "  -h, --help          show this help\n",
            program, DICE, SIDES);
}

static bool parse_uint64(const char *text, uint64_t *value)
{
    char *end;
    uintmax_t parsed;

    if (*text == '\0' || *text == '-') {
        return false;
    }
    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed > UINT64_MAX) {
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
            options->print_limit = 0;
            options->progress_seconds = 0;
            continue;
        }
        if (strcmp(argv[i], "--all-solutions") == 0) {
            options->print_limit = UINT64_MAX;
            continue;
        }
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            uint64_t value;

            if (++i >= argc || !parse_uint64(argv[i], &value) ||
                value > MAX_THREADS) {
                fprintf(stderr, "Invalid worker-thread count.\n");
                return false;
            }
            options->threads = (unsigned)value;
            continue;
        }
        if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--jobs") == 0) {
            if (++i >= argc || !parse_uint64(argv[i], &options->jobs) ||
                options->jobs == 0) {
                fprintf(stderr, "Invalid logical-job count.\n");
                return false;
            }
            continue;
        }
        if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--limit") == 0) {
            if (++i >= argc || !parse_uint64(argv[i], &options->limit)) {
                fprintf(stderr, "Invalid final-configuration limit.\n");
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
            if (++i >= argc ||
                !parse_uint64(argv[i], &options->print_limit)) {
                fprintf(stderr, "Invalid print limit.\n");
                return false;
            }
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return false;
    }
    return true;
}

static bool integer_power(unsigned base, unsigned exponent, uint64_t *result)
{
    uint64_t value = 1;
    unsigned i;

    for (i = 0; i < exponent; ++i) {
        if (value > UINT64_MAX / base) {
            return false;
        }
        value *= base;
    }
    *result = value;
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
            generate_perm_states(
                counter, target_length, depth + 1U, mask | bit,
                key | ((uint64_t)(die + 1U) << (4U * depth)), next);
        }
    }
}

static int find_perm_state(const struct perm_counter *counter,
                           unsigned length, uint64_t key)
{
    size_t state;

    for (state = counter->length_begin[length];
         state < counter->length_begin[length + 1U]; ++state) {
        if (counter->state[state].key == key) {
            return (int)state;
        }
    }
    return -1;
}

static bool initialize_perm_counter(struct perm_counter *counter)
{
    size_t next = 0;
    unsigned length;
    unsigned die;

    for (length = 0; length <= DICE; ++length) {
        counter->length_begin[length] = next;
        generate_perm_states(counter, length, 0, 0, 0, &next);
    }
    counter->length_begin[DICE + 1U] = next;
    if (next != PERM_STATE_COUNT) {
        return false;
    }

    for (die = 0; die < DICE; ++die) {
        size_t edge_count = 0;

        next = PERM_STATE_COUNT;
        while (next-- > 0) {
            const struct perm_state *source = &counter->state[next];
            uint64_t key;
            int destination;

            if ((source->mask & (1U << die)) != 0 ||
                source->length == DICE) {
                continue;
            }
            key = source->key |
                ((uint64_t)(die + 1U) << (4U * source->length));
            destination = find_perm_state(counter, source->length + 1U, key);
            if (destination < 0 || edge_count >= PERM_EDGE_COUNT) {
                return false;
            }
            counter->edge[die][edge_count++] = (struct perm_edge){
                .source = (uint16_t)next,
                .destination = (uint16_t)destination,
            };
        }
        if (edge_count != PERM_EDGE_COUNT) {
            return false;
        }
    }
    return true;
}

static void add_owner_to_ways(const struct perm_counter *counter,
                              uint64_t ways[PERM_STATE_COUNT],
                              unsigned owner)
{
    size_t edge;

    for (edge = 0; edge < PERM_EDGE_COUNT; ++edge) {
        const struct perm_edge *transition = &counter->edge[owner][edge];

        ways[transition->destination] += ways[transition->source];
    }
}

static bool allocate_stage_tables(struct weave_stage *stage)
{
    size_t entries;
    size_t place_entries;
    size_t pair_entries;
    size_t triple_entries;
    size_t difference_entries;

    if (!stage->possible) {
        return true;
    }
    if (stage->gap_count > SIZE_MAX / stage->permutation_count) {
        return false;
    }
    entries = (size_t)stage->gap_count * stage->permutation_count;
    if (entries > SIZE_MAX / sizeof(uint64_t)) {
        return false;
    }
    place_entries = (size_t)stage->gap_count * stage->dice;
    pair_entries = (size_t)stage->gap_count * (stage->dice - 1U);
    triple_entries = (size_t)stage->gap_count *
        stage->triple_projection_count;
    difference_entries = (size_t)stage->gap_count *
        (stage->permutation_count - 1U);
    stage->contribution = calloc(entries, sizeof(uint64_t));
    stage->minimum_from = calloc(entries, sizeof(uint64_t));
    stage->maximum_from = calloc(entries, sizeof(uint64_t));
    stage->gcd_from = calloc(entries, sizeof(uint64_t));
    stage->place_contribution = calloc(place_entries, sizeof(uint64_t));
    stage->place_minimum_from = calloc(place_entries, sizeof(uint64_t));
    stage->place_maximum_from = calloc(place_entries, sizeof(uint64_t));
    stage->place_gcd_from = calloc(place_entries, sizeof(uint64_t));
    if (stage->dice >= 2U) {
        stage->pair_contribution = calloc(pair_entries, sizeof(uint16_t));
    }
    if (stage->triple_projection_count != 0) {
        stage->triple_difference_contribution = calloc(
            triple_entries, sizeof(triple_projection_t));
        stage->triple_difference_minimum_from = calloc(
            triple_entries, sizeof(triple_projection_t));
        stage->triple_difference_maximum_from = calloc(
            triple_entries, sizeof(triple_projection_t));
    }
    if (stage->difference_bounds_possible) {
        stage->difference_contribution = calloc(
            difference_entries, sizeof(int64_t));
        stage->difference_minimum_from = calloc(
            difference_entries, sizeof(int64_t));
        stage->difference_maximum_from = calloc(
            difference_entries, sizeof(int64_t));
        stage->difference_gcd_from = calloc(
            difference_entries, sizeof(uint64_t));
    }
    return stage->contribution != NULL && stage->minimum_from != NULL &&
           stage->maximum_from != NULL && stage->gcd_from != NULL &&
           stage->place_contribution != NULL &&
           stage->place_minimum_from != NULL &&
           stage->place_maximum_from != NULL &&
           stage->place_gcd_from != NULL &&
           (stage->dice < 2U || stage->pair_contribution != NULL) &&
           (stage->triple_projection_count == 0 ||
            (stage->triple_difference_contribution != NULL &&
             stage->triple_difference_minimum_from != NULL &&
             stage->triple_difference_maximum_from != NULL)) &&
           (!stage->difference_bounds_possible ||
            (stage->difference_contribution != NULL &&
             stage->difference_minimum_from != NULL &&
             stage->difference_maximum_from != NULL &&
             stage->difference_gcd_from != NULL));
}

static bool record_stage_permutation(struct search *search,
                                     struct weave_stage *stage,
                                     const unsigned order[DICE],
                                     unsigned permutation)
{
    unsigned new_die = stage->dice - 1U;
    unsigned new_position = stage->dice;
    unsigned prefix_length = 0;
    unsigned suffix_length = 0;
    uint64_t prefix_key = 0;
    uint64_t reverse_suffix_key = 0;
    uint64_t full_key = 0;
    unsigned position;
    int state;

    for (position = 0; position < stage->dice; ++position) {
        full_key |= (uint64_t)(order[position] + 1U) << (4U * position);
        if (order[position] == new_die) {
            new_position = position;
            break;
        }
        prefix_key |=
            (uint64_t)(order[position] + 1U) << (4U * prefix_length++);
    }
    if (new_position == stage->dice) {
        return false;
    }
    stage->new_die_position[permutation] = (uint8_t)new_position;
    for (position = new_position + 1U; position < stage->dice; ++position) {
        full_key |= (uint64_t)(order[position] + 1U) << (4U * position);
    }
    position = stage->dice;
    while (position-- > new_position + 1U) {
        reverse_suffix_key |=
            (uint64_t)(order[position] + 1U) << (4U * suffix_length++);
    }

    state = find_perm_state(&search->permutations, prefix_length, prefix_key);
    if (state < 0) {
        return false;
    }
    stage->prefix_state[permutation] = (uint16_t)state;
    state = find_perm_state(&search->permutations, suffix_length,
                            reverse_suffix_key);
    if (state < 0) {
        return false;
    }
    stage->reverse_suffix_state[permutation] = (uint16_t)state;
    state = find_perm_state(&search->permutations, stage->dice, full_key);
    if (state < 0) {
        return false;
    }
    stage->full_state[permutation] = (uint16_t)state;
    return true;
}

static bool generate_stage_permutations(struct search *search,
                                        struct weave_stage *stage,
                                        unsigned depth, unsigned mask,
                                        unsigned order[DICE],
                                        unsigned *next)
{
    unsigned die;

    if (depth == stage->dice) {
        if (*next >= stage->permutation_count ||
            !record_stage_permutation(search, stage, order, *next)) {
            return false;
        }
        ++*next;
        return true;
    }
    for (die = 0; die < stage->dice; ++die) {
        if ((mask & (1U << die)) == 0) {
            order[depth] = die;
            if (!generate_stage_permutations(
                    search, stage, depth + 1U, mask | (1U << die), order,
                    next)) {
                return false;
            }
        }
    }
    return true;
}

static bool initialize_stages(struct search *search)
{
    uint64_t factorial = 1;
    unsigned dice;

    for (dice = 1; dice <= DICE; ++dice) {
        struct weave_stage *stage = &search->stage[dice];
        uint64_t outcomes;
        unsigned order[DICE];
        unsigned permutation = 0;

        factorial *= dice;
        stage->dice = dice;
        stage->old_length = (dice - 1U) * SIDES;
        stage->gap_count = stage->old_length + 1U;
        stage->permutation_count = (unsigned)factorial;
        if (!integer_power(SIDES, dice, &outcomes)) {
            fprintf(stderr, "%u^%u exceeds the 64-bit counting range.\n",
                    SIDES, dice);
            return false;
        }
        stage->possible = outcomes % factorial == 0;
        if (!stage->possible) {
            continue;
        }
        stage->goal = outcomes / factorial;
        stage->place_goal = outcomes / dice;
        stage->triple_projection_count = dice >= 4U
            ? 5U * ((dice - 1U) * (dice - 2U) / 2U)
            : 0;
        stage->difference_bounds_possible = dice >= 2U &&
            outcomes <= (uint64_t)INT64_MAX / 4U;
        if (stage->permutation_count > MAX_PERMUTATIONS ||
            !generate_stage_permutations(search, stage, 0, 0, order,
                                         &permutation) ||
            permutation != stage->permutation_count ||
            !allocate_stage_tables(stage)) {
            return false;
        }
    }
    return true;
}

static void free_stages(struct search *search)
{
    unsigned dice;

    for (dice = 1; dice <= DICE; ++dice) {
        free(search->stage[dice].contribution);
        free(search->stage[dice].minimum_from);
        free(search->stage[dice].maximum_from);
        free(search->stage[dice].gcd_from);
        free(search->stage[dice].place_contribution);
        free(search->stage[dice].place_minimum_from);
        free(search->stage[dice].place_maximum_from);
        free(search->stage[dice].place_gcd_from);
        free(search->stage[dice].pair_contribution);
        free(search->stage[dice].triple_difference_contribution);
        free(search->stage[dice].triple_difference_minimum_from);
        free(search->stage[dice].triple_difference_maximum_from);
        free(search->stage[dice].difference_contribution);
        free(search->stage[dice].difference_minimum_from);
        free(search->stage[dice].difference_maximum_from);
        free(search->stage[dice].difference_gcd_from);
    }
    free(search->above);
}

static uint64_t gcd_uint64(uint64_t first, uint64_t second)
{
    while (second != 0) {
        uint64_t remainder = first % second;

        first = second;
        second = remainder;
    }
    return first;
}

static uint64_t unsigned_distance(uint64_t first, uint64_t second)
{
    return first >= second ? first - second : second - first;
}

static uint64_t signed_distance(int64_t first, int64_t second)
{
    return first >= second
        ? (uint64_t)(first - second)
        : (uint64_t)(second - first);
}

static bool prepare_stage(struct search *search, unsigned dice)
{
    struct weave_stage *stage = &search->stage[dice];
    const uint8_t *word = search->word[dice - 1U];
    unsigned permutation_count = stage->permutation_count;
    unsigned old_dice = dice - 1U;
    unsigned gap;
    unsigned permutation;
    unsigned first;
    unsigned second;
    uint64_t below_faces[DICE] = {0};
    uint64_t above_faces[DICE] = {0};
    uint64_t below_order[DICE][DICE] = {{0}};
    uint64_t above_order[DICE][DICE] = {{0}};
    uint64_t fixed_face_outcomes;

    if (!stage->possible) {
        return true;
    }
    if (!integer_power(SIDES, dice - 1U, &fixed_face_outcomes)) {
        return false;
    }

    if (dice >= 2U) {
        for (first = 0; first < old_dice; ++first) {
            above_faces[first] = SIDES;
        }
        for (gap = 0; gap < stage->old_length; ++gap) {
            unsigned owner = word[gap];

            if (owner >= old_dice) {
                return false;
            }
            for (first = 0; first < old_dice; ++first) {
                if (first != owner) {
                    above_order[first][owner] += below_faces[first];
                }
            }
            ++below_faces[owner];
        }
        memset(below_faces, 0, sizeof(below_faces));
    }

    memset(search->reverse_ways, 0, sizeof(search->reverse_ways));
    search->reverse_ways[0] = 1;
    gap = stage->gap_count;
    while (gap-- > 0) {
        size_t base = (size_t)gap * permutation_count;

        for (permutation = 0; permutation < permutation_count;
             ++permutation) {
            search->above[base + permutation] = search->reverse_ways[
                stage->reverse_suffix_state[permutation]];
        }
        if (gap != 0) {
            add_owner_to_ways(&search->permutations, search->reverse_ways,
                              word[gap - 1U]);
        }
    }

    memset(search->lower_ways, 0, sizeof(search->lower_ways));
    search->lower_ways[0] = 1;
    for (gap = 0; gap < stage->gap_count; ++gap) {
        size_t base = (size_t)gap * permutation_count;
        size_t place_base = (size_t)gap * dice;
        uint64_t sum = 0;

        memset(&stage->place_contribution[place_base], 0,
               dice * sizeof(stage->place_contribution[0]));
        for (permutation = 0; permutation < permutation_count;
             ++permutation) {
            uint64_t below = search->lower_ways[
                stage->prefix_state[permutation]];
            uint64_t above = search->above[base + permutation];
            uint64_t contribution;

            if (below != 0 && above > UINT64_MAX / below) {
                return false;
            }
            contribution = below * above;
            stage->contribution[base + permutation] = contribution;
            if (sum > UINT64_MAX - contribution) {
                return false;
            }
            sum += contribution;
            stage->place_contribution[
                place_base + stage->new_die_position[permutation]] +=
                    contribution;
        }
        if (stage->difference_bounds_possible) {
            size_t difference_base =
                (size_t)gap * (permutation_count - 1U);
            int64_t baseline = (int64_t)stage->contribution[base];

            for (permutation = 1; permutation < permutation_count;
                 ++permutation) {
                stage->difference_contribution[
                    difference_base + permutation - 1U] =
                        (int64_t)stage->contribution[base + permutation] -
                        baseline;
            }
        }
        if (sum != fixed_face_outcomes) {
            return false;
        }
        if (dice >= 2U) {
            size_t pair_base = (size_t)gap * old_dice;

            for (first = 0; first < old_dice; ++first) {
                stage->pair_contribution[pair_base + first] =
                    (uint16_t)below_faces[first];
            }
        }
        if (stage->triple_projection_count != 0) {
            size_t triple_base =
                (size_t)gap * stage->triple_projection_count;
            unsigned projection = 0;

            /*
             * For each pair of old dice, use the order new,first,second as
             * a baseline and store the other five order counts as signed
             * differences.  If all differences are zero, all six counts are
             * S^3 / 3!, so only five compact projections are needed.
             */
            for (first = 0; first < old_dice; ++first) {
                for (second = first + 1U; second < old_dice; ++second) {
                    int64_t baseline =
                        (int64_t)above_order[first][second];
                    int64_t contribution[5] = {
                        (int64_t)above_order[second][first],
                        (int64_t)(below_faces[first] * above_faces[second]),
                        (int64_t)(below_faces[second] * above_faces[first]),
                        (int64_t)below_order[first][second],
                        (int64_t)below_order[second][first],
                    };
                    unsigned index;

                    for (index = 0; index < 5U; ++index) {
                        stage->triple_difference_contribution[
                            triple_base + projection++] =
                                (triple_projection_t)
                                    (contribution[index] - baseline);
                    }
                }
            }
            if (projection != stage->triple_projection_count) {
                return false;
            }
        }
        if (gap < stage->old_length) {
            unsigned owner = word[gap];

            for (first = 0; first < old_dice; ++first) {
                if (first != owner) {
                    above_order[owner][first] -= above_faces[first];
                    below_order[first][owner] += below_faces[first];
                }
            }
            --above_faces[owner];
            ++below_faces[owner];
            add_owner_to_ways(&search->permutations, search->lower_ways,
                              owner);
        }
    }

    gap = stage->gap_count;
    while (gap-- > 0) {
        size_t base = (size_t)gap * permutation_count;
        size_t place_base = (size_t)gap * dice;

        for (permutation = 0; permutation < permutation_count;
             ++permutation) {
            uint64_t value = stage->contribution[base + permutation];

            if (gap + 1U == stage->gap_count) {
                stage->minimum_from[base + permutation] = value;
                stage->maximum_from[base + permutation] = value;
                stage->gcd_from[base + permutation] = 0;
            } else {
                size_t next = base + permutation_count + permutation;
                uint64_t next_minimum = stage->minimum_from[next];
                uint64_t next_maximum = stage->maximum_from[next];
                uint64_t next_value = stage->contribution[
                    base + permutation_count + permutation];

                stage->minimum_from[base + permutation] =
                    value < next_minimum ? value : next_minimum;
                stage->maximum_from[base + permutation] =
                    value > next_maximum ? value : next_maximum;
                stage->gcd_from[base + permutation] = gcd_uint64(
                    stage->gcd_from[next],
                    unsigned_distance(value, next_value));
            }
        }
        for (permutation = 0; permutation < dice; ++permutation) {
            uint64_t value =
                stage->place_contribution[place_base + permutation];

            if (gap + 1U == stage->gap_count) {
                stage->place_minimum_from[place_base + permutation] = value;
                stage->place_maximum_from[place_base + permutation] = value;
                stage->place_gcd_from[place_base + permutation] = 0;
            } else {
                size_t next = place_base + dice + permutation;
                uint64_t next_minimum = stage->place_minimum_from[next];
                uint64_t next_maximum = stage->place_maximum_from[next];
                uint64_t next_value = stage->place_contribution[
                    place_base + dice + permutation];

                stage->place_minimum_from[place_base + permutation] =
                    value < next_minimum ? value : next_minimum;
                stage->place_maximum_from[place_base + permutation] =
                    value > next_maximum ? value : next_maximum;
                stage->place_gcd_from[place_base + permutation] = gcd_uint64(
                    stage->place_gcd_from[next],
                    unsigned_distance(value, next_value));
            }
        }
        if (stage->triple_projection_count != 0) {
            size_t triple_base =
                (size_t)gap * stage->triple_projection_count;

            for (permutation = 0;
                 permutation < stage->triple_projection_count;
                 ++permutation) {
                triple_projection_t value =
                    stage->triple_difference_contribution[
                        triple_base + permutation];

                if (gap + 1U == stage->gap_count) {
                    stage->triple_difference_minimum_from[
                        triple_base + permutation] = value;
                    stage->triple_difference_maximum_from[
                        triple_base + permutation] = value;
                } else {
                    size_t next = triple_base +
                        stage->triple_projection_count + permutation;
                    triple_projection_t next_minimum =
                        stage->triple_difference_minimum_from[next];
                    triple_projection_t next_maximum =
                        stage->triple_difference_maximum_from[next];

                    stage->triple_difference_minimum_from[
                        triple_base + permutation] =
                            value < next_minimum ? value : next_minimum;
                    stage->triple_difference_maximum_from[
                        triple_base + permutation] =
                            value > next_maximum ? value : next_maximum;
                }
            }
        }
        if (stage->difference_bounds_possible) {
            unsigned difference_count = permutation_count - 1U;
            size_t difference_base = (size_t)gap * difference_count;

            for (permutation = 0; permutation < difference_count;
                 ++permutation) {
                int64_t value = stage->difference_contribution[
                    difference_base + permutation];

                if (gap + 1U == stage->gap_count) {
                    stage->difference_minimum_from[
                        difference_base + permutation] = value;
                    stage->difference_maximum_from[
                        difference_base + permutation] = value;
                    stage->difference_gcd_from[
                        difference_base + permutation] = 0;
                } else {
                    size_t next =
                        difference_base + difference_count + permutation;
                    int64_t next_minimum =
                        stage->difference_minimum_from[next];
                    int64_t next_maximum =
                        stage->difference_maximum_from[next];
                    int64_t next_value = stage->difference_contribution[
                        difference_base + difference_count + permutation];

                    stage->difference_minimum_from[
                        difference_base + permutation] =
                            value < next_minimum ? value : next_minimum;
                    stage->difference_maximum_from[
                        difference_base + permutation] =
                            value > next_maximum ? value : next_maximum;
                    stage->difference_gcd_from[
                        difference_base + permutation] = gcd_uint64(
                            stage->difference_gcd_from[next],
                            signed_distance(value, next_value));
                }
            }
        }
    }
    memset(search->tally[dice], 0,
           permutation_count * sizeof(search->tally[dice][0]));
    memset(search->place_tally[dice], 0,
           dice * sizeof(search->place_tally[dice][0]));
    if (dice >= 2U) {
        memset(search->pair_tally[dice], 0,
               old_dice * sizeof(search->pair_tally[dice][0]));
    }
    if (stage->triple_projection_count != 0) {
        memset(search->triple_difference_tally[dice], 0,
               stage->triple_projection_count *
                   sizeof(search->triple_difference_tally[dice][0]));
    }
    return true;
}

static bool bounds_allow_goal(const struct search *search, unsigned dice,
                              unsigned faces_used, unsigned minimum_gap)
{
    const struct weave_stage *stage = &search->stage[dice];
    unsigned remaining = SIDES - faces_used;
    size_t base = (size_t)minimum_gap * stage->permutation_count;
    unsigned permutation;

    for (permutation = 0; permutation < stage->permutation_count;
         ++permutation) {
        uint64_t current = search->tally[dice][permutation];
        uint64_t required;
        uint64_t minimum;
        uint64_t maximum;
        uint64_t required_average_ceiling;

        if (current > stage->goal) {
            return false;
        }
        required = stage->goal - current;
        if (remaining == 0) {
            if (required != 0) {
                return false;
            }
            continue;
        }
        minimum = stage->minimum_from[base + permutation];
        maximum = stage->maximum_from[base + permutation];
        if (minimum > required / remaining) {
            return false;
        }
        required_average_ceiling = required / remaining +
            (required % remaining != 0);
        if (maximum < required_average_ceiling) {
            return false;
        }
    }
    return true;
}

static bool place_projection_bounds_allow_goal(
    const struct search *search, unsigned dice, unsigned faces_used,
    unsigned minimum_gap)
{
    const struct weave_stage *stage = &search->stage[dice];
    unsigned remaining = SIDES - faces_used;
    size_t base = (size_t)minimum_gap * dice;
    unsigned place;

    for (place = 0; place < dice; ++place) {
        uint64_t current = search->place_tally[dice][place];
        uint64_t required;
        uint64_t minimum;
        uint64_t maximum;

        if (current > stage->place_goal) {
            return false;
        }
        required = stage->place_goal - current;
        if (remaining == 0) {
            if (required != 0) {
                return false;
            }
            continue;
        }
        minimum = stage->place_minimum_from[base + place];
        maximum = stage->place_maximum_from[base + place];
        if (minimum > required / remaining ||
            maximum < required / remaining +
                (required % remaining != 0)) {
            return false;
        }
    }
    return true;
}

static bool pairwise_bounds_allow_goal(const struct search *search,
                                       unsigned dice,
                                       unsigned faces_used,
                                       unsigned minimum_gap)
{
    const struct weave_stage *stage = &search->stage[dice];
    unsigned old_dice = dice - 1U;
    unsigned remaining = SIDES - faces_used;
    size_t base = (size_t)minimum_gap * old_dice;
    uint64_t goal = (uint64_t)SIDES * SIDES / 2U;
    unsigned old_die;

    for (old_die = 0; old_die < old_dice; ++old_die) {
        uint64_t current = search->pair_tally[dice][old_die];
        uint64_t required;

        if (current > goal) {
            return false;
        }
        required = goal - current;
        if (remaining == 0) {
            if (required != 0) {
                return false;
            }
            continue;
        }
        if (stage->pair_contribution[base + old_die] >
                required / remaining ||
            SIDES < required / remaining +
                (required % remaining != 0)) {
            return false;
        }
    }
    return true;
}

static bool triplet_bounds_allow_goal(const struct search *search,
                                      unsigned dice,
                                      unsigned faces_used,
                                      unsigned minimum_gap)
{
    const struct weave_stage *stage = &search->stage[dice];
    unsigned remaining = SIDES - faces_used;
    size_t base =
        (size_t)minimum_gap * stage->triple_projection_count;
    unsigned projection;

    for (projection = 0;
         projection < stage->triple_projection_count;
         ++projection) {
        int64_t current =
            search->triple_difference_tally[dice][projection];

        if (remaining == 0) {
            if (current != 0) {
                return false;
            }
        } else if (current + (int64_t)remaining *
                       stage->triple_difference_minimum_from[
                           base + projection] > 0 ||
                   current + (int64_t)remaining *
                       stage->triple_difference_maximum_from[
                           base + projection] < 0) {
            return false;
        }
    }
    return true;
}

static bool projection_bounds_allow_goal(struct search *search,
                                         unsigned dice,
                                         unsigned faces_used,
                                         unsigned minimum_gap)
{
    if (!place_projection_bounds_allow_goal(
            search, dice, faces_used, minimum_gap)) {
        ++search->place_projection_prunes[dice];
        return false;
    }
    if (!pairwise_bounds_allow_goal(
            search, dice, faces_used, minimum_gap)) {
        ++search->pairwise_prunes[dice];
        return false;
    }
    if (!triplet_bounds_allow_goal(
            search, dice, faces_used, minimum_gap)) {
        ++search->triplet_prunes[dice];
        return false;
    }
    return true;
}

static bool difference_projection_bounds_allow_goal(
    const struct search *search, unsigned dice, unsigned faces_used,
    unsigned minimum_gap)
{
    const struct weave_stage *stage = &search->stage[dice];
    unsigned difference_count = stage->permutation_count - 1U;
    unsigned remaining = SIDES - faces_used;
    size_t base = (size_t)minimum_gap * difference_count;
    unsigned difference;
    int64_t baseline;

    if (!stage->difference_bounds_possible) {
        return true;
    }
    baseline = (int64_t)search->tally[dice][0];
    for (difference = 0; difference < difference_count; ++difference) {
        int64_t current =
            (int64_t)search->tally[dice][difference + 1U] - baseline;

        if (remaining == 0) {
            if (current != 0) {
                return false;
            }
        } else if (current + (int64_t)remaining *
                       stage->difference_minimum_from[base + difference] > 0 ||
                   current + (int64_t)remaining *
                       stage->difference_maximum_from[base + difference] < 0) {
            return false;
        }
    }
    return true;
}

static bool congruence_bounds_allow_goal(const struct search *search,
                                         unsigned dice,
                                         unsigned faces_used,
                                         unsigned minimum_gap)
{
    const struct weave_stage *stage = &search->stage[dice];
    unsigned remaining = SIDES - faces_used;
    size_t permutation_base =
        (size_t)minimum_gap * stage->permutation_count;
    size_t place_base = (size_t)minimum_gap * dice;
    unsigned index;

    if (remaining == 0) {
        return true;
    }
    for (index = 0; index < dice; ++index) {
        uint64_t divisor = stage->place_gcd_from[place_base + index];

        if (divisor != 0) {
            uint64_t required =
                stage->place_goal - search->place_tally[dice][index];
            uint64_t reference =
                stage->place_contribution[place_base + index];

            if (unsigned_distance(required,
                                  (uint64_t)remaining * reference) %
                    divisor != 0) {
                return false;
            }
        }
    }
    for (index = 0; index < stage->permutation_count; ++index) {
        uint64_t divisor = stage->gcd_from[permutation_base + index];

        if (divisor != 0) {
            uint64_t required =
                stage->goal - search->tally[dice][index];
            uint64_t reference =
                stage->contribution[permutation_base + index];

            if (unsigned_distance(required,
                                  (uint64_t)remaining * reference) %
                    divisor != 0) {
                return false;
            }
        }
    }
    if (stage->difference_bounds_possible) {
        unsigned difference_count = stage->permutation_count - 1U;
        size_t difference_base = (size_t)minimum_gap * difference_count;
        int64_t baseline = (int64_t)search->tally[dice][0];

        for (index = 0; index < difference_count; ++index) {
            uint64_t divisor =
                stage->difference_gcd_from[difference_base + index];

            if (divisor != 0) {
                int64_t current =
                    (int64_t)search->tally[dice][index + 1U] - baseline;
                int64_t required = -current;
                int64_t reference =
                    stage->difference_contribution[difference_base + index];

                if (signed_distance(required,
                                    (int64_t)remaining * reference) %
                        divisor != 0) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool verify_configuration(struct search *search, unsigned dice)
{
    const struct weave_stage *stage = &search->stage[dice];
    unsigned face;
    unsigned permutation;

    memset(search->permutations.ways, 0,
           sizeof(search->permutations.ways));
    search->permutations.ways[0] = 1;
    for (face = 0; face < dice * SIDES; ++face) {
        add_owner_to_ways(&search->permutations,
                          search->permutations.ways,
                          search->word[dice][face]);
    }
    for (permutation = 0; permutation < stage->permutation_count;
         ++permutation) {
        if (search->permutations.ways[stage->full_state[permutation]] !=
            stage->goal) {
            return false;
        }
    }
    return true;
}

static void print_configuration(const struct queued_solution *solution)
{
    printf("permutation-fair #%" PRIu64 " depth=%dd%d encoding=%s\n",
           solution->number, DICE, SIDES, solution->encoding);
}

static void enqueue_solution(struct search *search, uint64_t number)
{
    struct shared_state *shared = search->shared;
    struct queued_solution *solution;
    unsigned face;

    pthread_mutex_lock(&shared->solution_mutex);
    while (shared->solution_count == SOLUTION_QUEUE_CAPACITY) {
        pthread_cond_wait(&shared->solution_not_full,
                          &shared->solution_mutex);
    }
    solution = &shared->solution_queue[shared->solution_tail];
    solution->number = number;
    for (face = 0; face < FACE_COUNT; ++face) {
        solution->encoding[face] =
            (char)('a' + search->word[DICE][face]);
    }
    solution->encoding[FACE_COUNT] = '\0';
    shared->solution_tail =
        (shared->solution_tail + 1U) % SOLUTION_QUEUE_CAPACITY;
    ++shared->solution_count;
    pthread_mutex_unlock(&shared->solution_mutex);
}

static void drain_solutions(struct shared_state *shared)
{
    for (;;) {
        struct queued_solution solution;
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
        print_configuration(&solution);
    }
    fflush(stdout);
}

static bool claim_solution(struct shared_state *shared, uint64_t *number)
{
    if (shared->options.limit == 0) {
        *number = atomic_fetch_add_explicit(
            &shared->solution_total, 1, memory_order_relaxed) + 1U;
        return true;
    }
    for (;;) {
        uint64_t current = atomic_load_explicit(
            &shared->solution_total, memory_order_relaxed);

        if (current >= shared->options.limit) {
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &shared->solution_total, &current, current + 1U,
                memory_order_relaxed, memory_order_relaxed)) {
            *number = current + 1U;
            return true;
        }
    }
}

static void publish_stats(const struct search *search)
{
    unsigned dice;

    for (dice = 1; dice <= DICE; ++dice) {
        atomic_store_explicit(&search->published->configurations[dice],
                              search->configurations[dice],
                              memory_order_relaxed);
        atomic_store_explicit(&search->published->nodes[dice],
                              search->nodes[dice], memory_order_relaxed);
        atomic_store_explicit(&search->published->label_symmetry_skips[dice],
                              search->label_symmetry_skips[dice],
                              memory_order_relaxed);
        atomic_store_explicit(
            &search->published->place_projection_prunes[dice],
            search->place_projection_prunes[dice], memory_order_relaxed);
        atomic_store_explicit(&search->published->pairwise_prunes[dice],
                              search->pairwise_prunes[dice],
                              memory_order_relaxed);
        atomic_store_explicit(&search->published->triplet_prunes[dice],
                              search->triplet_prunes[dice],
                              memory_order_relaxed);
        atomic_store_explicit(
            &search->published->difference_projection_prunes[dice],
            search->difference_projection_prunes[dice],
            memory_order_relaxed);
        atomic_store_explicit(&search->published->bound_prunes[dice],
                              search->bound_prunes[dice],
                              memory_order_relaxed);
        atomic_store_explicit(&search->published->congruence_prunes[dice],
                              search->congruence_prunes[dice],
                              memory_order_relaxed);
    }
}

static void maybe_publish_stats(struct search *search)
{
    if ((search->total_nodes & PROGRESS_CHECK_MASK) == 0) {
        publish_stats(search);
    }
}

static void search_insertions(struct search *search, unsigned dice,
                              unsigned face, unsigned minimum_gap);

static void start_canonical_stage(struct search *search, unsigned dice)
{
    const struct weave_stage *stage = &search->stage[dice];
    unsigned permutation;

    /*
     * The first face of every newly added die is canonically fixed at gap
     * zero.  Seed that choice before entering the generic recursion so its
     * hot loop never considers or tests noncanonical label orders.
     */
    search->label_symmetry_skips[dice] += stage->gap_count - 1U;
    search->insertion_gap[dice][0] = 0;
    for (permutation = 0; permutation < dice; ++permutation) {
        search->place_tally[dice][permutation] +=
            stage->place_contribution[permutation];
    }
    for (permutation = 0; permutation < dice - 1U; ++permutation) {
        search->pair_tally[dice][permutation] +=
            stage->pair_contribution[permutation];
    }
    for (permutation = 0;
         permutation < stage->triple_projection_count;
         ++permutation) {
        search->triple_difference_tally[dice][permutation] +=
            stage->triple_difference_contribution[permutation];
    }
    if (projection_bounds_allow_goal(search, dice, 1, 0)) {
        for (permutation = 0; permutation < stage->permutation_count;
             ++permutation) {
            search->tally[dice][permutation] +=
                stage->contribution[permutation];
        }
        search_insertions(search, dice, 1, 0);
        for (permutation = 0; permutation < stage->permutation_count;
             ++permutation) {
            search->tally[dice][permutation] -=
                stage->contribution[permutation];
        }
    }
    for (permutation = 0;
         permutation < stage->triple_projection_count;
         ++permutation) {
        search->triple_difference_tally[dice][permutation] -=
            stage->triple_difference_contribution[permutation];
    }
    for (permutation = 0; permutation < dice - 1U; ++permutation) {
        search->pair_tally[dice][permutation] -=
            stage->pair_contribution[permutation];
    }
    for (permutation = 0; permutation < dice; ++permutation) {
        search->place_tally[dice][permutation] -=
            stage->place_contribution[permutation];
    }
}

static void accept_extension(struct search *search, unsigned dice)
{
    const struct weave_stage *stage = &search->stage[dice];
    const uint8_t *old_word = search->word[dice - 1U];
    uint8_t *new_word = search->word[dice];
    unsigned old_face = 0;
    unsigned new_face = 0;
    unsigned output = 0;

    while (old_face < stage->old_length || new_face < SIDES) {
        if (new_face < SIDES &&
            search->insertion_gap[dice][new_face] == old_face) {
            new_word[output++] = (uint8_t)(dice - 1U);
            ++new_face;
        } else {
            new_word[output++] = old_word[old_face++];
        }
    }
    if (output != dice * SIDES || !verify_configuration(search, dice)) {
        search->internal_error = true;
        atomic_store_explicit(&search->shared->internal_error, true,
                              memory_order_relaxed);
        atomic_store_explicit(&search->shared->stop, true,
                              memory_order_relaxed);
        return;
    }

    if (dice == DICE) {
        uint64_t number;

        if (!claim_solution(search->shared, &number)) {
            return;
        }
        ++search->configurations[dice];
        if (number <= search->options.print_limit) {
            enqueue_solution(search, number);
        }
        if (search->options.limit != 0 && number >= search->options.limit) {
            search->hit_limit = true;
            atomic_store_explicit(&search->shared->stop, true,
                                  memory_order_relaxed);
        }
        return;
    }
    ++search->configurations[dice];
    if (search->stage[dice + 1U].possible) {
        if (!prepare_stage(search, dice + 1U)) {
            search->internal_error = true;
            atomic_store_explicit(&search->shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&search->shared->stop, true,
                                  memory_order_relaxed);
            return;
        }
        start_canonical_stage(search, dice + 1U);
    }
}

static void search_insertions(struct search *search, unsigned dice,
                              unsigned face, unsigned minimum_gap)
{
    const struct weave_stage *stage = &search->stage[dice];
    unsigned gap;

    if (interrupt_requested || search->hit_limit || search->internal_error ||
        atomic_load_explicit(&search->shared->stop, memory_order_relaxed)) {
        return;
    }
    ++search->nodes[dice];
    ++search->total_nodes;
    maybe_publish_stats(search);

    if (!difference_projection_bounds_allow_goal(
            search, dice, face, minimum_gap)) {
        ++search->difference_projection_prunes[dice];
        return;
    }
    if (!bounds_allow_goal(search, dice, face, minimum_gap)) {
        ++search->bound_prunes[dice];
        return;
    }
    if (!congruence_bounds_allow_goal(search, dice, face, minimum_gap)) {
        ++search->congruence_prunes[dice];
        return;
    }
    if (face == SIDES) {
        accept_extension(search, dice);
        return;
    }

    for (gap = minimum_gap; gap < stage->gap_count; ++gap) {
        size_t base = (size_t)gap * stage->permutation_count;
        size_t place_base = (size_t)gap * dice;
        size_t pair_base = (size_t)gap * (dice - 1U);
        size_t triple_base =
            (size_t)gap * stage->triple_projection_count;
        unsigned permutation;

        search->insertion_gap[dice][face] = (uint16_t)gap;
        for (permutation = 0; permutation < dice; ++permutation) {
            search->place_tally[dice][permutation] +=
                stage->place_contribution[place_base + permutation];
        }
        for (permutation = 0; permutation < dice - 1U; ++permutation) {
            search->pair_tally[dice][permutation] +=
                stage->pair_contribution[pair_base + permutation];
        }
        for (permutation = 0;
             permutation < stage->triple_projection_count;
             ++permutation) {
            search->triple_difference_tally[dice][permutation] +=
                stage->triple_difference_contribution[
                    triple_base + permutation];
        }
        if (projection_bounds_allow_goal(
                search, dice, face + 1U, gap)) {
            for (permutation = 0;
                 permutation < stage->permutation_count;
                 ++permutation) {
                search->tally[dice][permutation] +=
                    stage->contribution[base + permutation];
            }
            search_insertions(search, dice, face + 1U, gap);
            for (permutation = 0;
                 permutation < stage->permutation_count;
                 ++permutation) {
                search->tally[dice][permutation] -=
                    stage->contribution[base + permutation];
            }
        }
        for (permutation = 0;
             permutation < stage->triple_projection_count;
             ++permutation) {
            search->triple_difference_tally[dice][permutation] -=
                stage->triple_difference_contribution[
                    triple_base + permutation];
        }
        for (permutation = 0; permutation < dice - 1U; ++permutation) {
            search->pair_tally[dice][permutation] -=
                stage->pair_contribution[pair_base + permutation];
        }
        for (permutation = 0; permutation < dice; ++permutation) {
            search->place_tally[dice][permutation] -=
                stage->place_contribution[place_base + permutation];
        }
        if (interrupt_requested || search->hit_limit ||
            search->internal_error ||
            atomic_load_explicit(&search->shared->stop,
                                 memory_order_relaxed)) {
            return;
        }
    }
}

static uint64_t binomial_coefficient(unsigned n, unsigned k)
{
    uint64_t value = 1;
    unsigned i;

    if (k > n) {
        return 0;
    }
    if (k > n - k) {
        k = n - k;
    }
    for (i = 1; i <= k; ++i) {
        uint64_t numerator = n - k + i;

        if (value > UINT64_MAX / numerator) {
            return UINT64_MAX;
        }
        value = value * numerator / i;
    }
    return value;
}

static uint64_t prefix_count_for_faces(unsigned prefix_faces)
{
    unsigned free_faces = prefix_faces - 1U;

    return binomial_coefficient(SIDES + free_faces, free_faces);
}

static bool decode_prefix(uint64_t rank, unsigned prefix_faces,
                          uint16_t gaps[SIDES])
{
    unsigned minimum = 0;
    unsigned face;

    gaps[0] = 0;
    for (face = 1; face < prefix_faces; ++face) {
        unsigned remaining = prefix_faces - face - 1U;
        unsigned gap;

        for (gap = minimum; gap <= SIDES; ++gap) {
            uint64_t count = binomial_coefficient(
                SIDES - gap + remaining, remaining);

            if (rank < count) {
                gaps[face] = (uint16_t)gap;
                minimum = gap;
                break;
            }
            rank -= count;
        }
        if (gap > SIDES) {
            return false;
        }
    }
    return rank == 0;
}

static void apply_stage_two_prefix(struct search *search, uint64_t prefix)
{
    struct weave_stage *stage = &search->stage[2];
    unsigned face;

    memset(search->tally[2], 0,
           stage->permutation_count * sizeof(search->tally[2][0]));
    memset(search->place_tally[2], 0,
           2U * sizeof(search->place_tally[2][0]));
    search->pair_tally[2][0] = 0;
    if (!decode_prefix(prefix, search->shared->prefix_faces,
                       search->insertion_gap[2])) {
        search->internal_error = true;
        atomic_store_explicit(&search->shared->internal_error, true,
                              memory_order_relaxed);
        atomic_store_explicit(&search->shared->stop, true,
                              memory_order_relaxed);
        return;
    }
    for (face = 0; face < search->shared->prefix_faces; ++face) {
        unsigned gap = search->insertion_gap[2][face];
        unsigned index;

        for (index = 0; index < 2U; ++index) {
            search->place_tally[2][index] +=
                stage->place_contribution[(size_t)gap * 2U + index];
        }
        search->pair_tally[2][0] += stage->pair_contribution[gap];
    }
    if (!projection_bounds_allow_goal(
            search, 2, search->shared->prefix_faces,
            search->insertion_gap[2][search->shared->prefix_faces - 1U])) {
        return;
    }
    for (face = 0; face < search->shared->prefix_faces; ++face) {
        unsigned gap = search->insertion_gap[2][face];
        size_t base = (size_t)gap * stage->permutation_count;
        unsigned index;

        for (index = 0; index < stage->permutation_count; ++index) {
            search->tally[2][index] +=
                stage->contribution[base + index];
        }
    }
    search_insertions(
        search, 2, search->shared->prefix_faces,
        search->insertion_gap[2][search->shared->prefix_faces - 1U]);
}

static void *worker_main(void *argument)
{
    struct worker *worker = argument;
    struct shared_state *shared = worker->shared;
    struct search *search = worker->search;

    for (;;) {
        uint64_t job = atomic_fetch_add_explicit(
            &shared->next_job, 1, memory_order_relaxed);
        uint64_t prefix;

        if (job >= shared->job_count || interrupt_requested ||
            atomic_load_explicit(&shared->stop, memory_order_relaxed)) {
            break;
        }
        if (job == 0) {
            search->label_symmetry_skips[2] = SIDES;
        }
        for (prefix = job; prefix < shared->prefix_count;
             prefix += shared->job_count) {
            apply_stage_two_prefix(search, prefix);
            if (interrupt_requested || search->internal_error ||
                atomic_load_explicit(&shared->stop, memory_order_relaxed)) {
                break;
            }
            if (prefix > UINT64_MAX - shared->job_count) {
                break;
            }
        }
        if (!search->internal_error && !interrupt_requested &&
            !atomic_load_explicit(&shared->stop, memory_order_relaxed)) {
            atomic_fetch_add_explicit(&shared->jobs_done, 1,
                                      memory_order_relaxed);
        }
        publish_stats(search);
    }
    if (interrupt_requested) {
        atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
    }
    publish_stats(search);
    atomic_fetch_sub_explicit(&shared->active_workers, 1,
                              memory_order_relaxed);
    return NULL;
}

struct totals {
    uint64_t configurations[DICE + 1U];
    uint64_t nodes[DICE + 1U];
    uint64_t label_symmetry_skips[DICE + 1U];
    uint64_t place_projection_prunes[DICE + 1U];
    uint64_t pairwise_prunes[DICE + 1U];
    uint64_t triplet_prunes[DICE + 1U];
    uint64_t difference_projection_prunes[DICE + 1U];
    uint64_t bound_prunes[DICE + 1U];
    uint64_t congruence_prunes[DICE + 1U];
    uint64_t total_nodes;
};

static struct totals collect_totals(const struct worker *workers,
                                    unsigned worker_count)
{
    struct totals totals = {0};
    unsigned worker;
    unsigned dice;

    totals.configurations[1] = 1;
    for (worker = 0; worker < worker_count; ++worker) {
        const struct published_stats *stats = &workers[worker].stats;

        for (dice = 1; dice <= DICE; ++dice) {
            totals.configurations[dice] += atomic_load_explicit(
                &stats->configurations[dice], memory_order_relaxed);
            totals.nodes[dice] += atomic_load_explicit(
                &stats->nodes[dice], memory_order_relaxed);
            totals.label_symmetry_skips[dice] += atomic_load_explicit(
                &stats->label_symmetry_skips[dice], memory_order_relaxed);
            totals.place_projection_prunes[dice] += atomic_load_explicit(
                &stats->place_projection_prunes[dice],
                memory_order_relaxed);
            totals.pairwise_prunes[dice] += atomic_load_explicit(
                &stats->pairwise_prunes[dice], memory_order_relaxed);
            totals.triplet_prunes[dice] += atomic_load_explicit(
                &stats->triplet_prunes[dice], memory_order_relaxed);
            totals.difference_projection_prunes[dice] +=
                atomic_load_explicit(
                    &stats->difference_projection_prunes[dice],
                    memory_order_relaxed);
            totals.bound_prunes[dice] += atomic_load_explicit(
                &stats->bound_prunes[dice], memory_order_relaxed);
            totals.congruence_prunes[dice] += atomic_load_explicit(
                &stats->congruence_prunes[dice], memory_order_relaxed);
        }
    }
    for (dice = 2; dice <= DICE; ++dice) {
        totals.total_nodes += totals.nodes[dice];
    }
    return totals;
}

static void print_totals(const char *label, const struct shared_state *shared,
                         const struct worker *workers, double start_time)
{
    struct totals totals = collect_totals(workers, shared->thread_count);
    uint64_t jobs_done = atomic_load_explicit(
        &shared->jobs_done, memory_order_relaxed);
    unsigned dice;

    fprintf(stderr,
            "%s: %.1fs, workers=%u, jobs=%" PRIu64 "/%" PRIu64
            ", nodes=%" PRIu64 ", configurations=",
            label, monotonic_seconds() - start_time, shared->thread_count,
            jobs_done, shared->job_count, totals.total_nodes);
    for (dice = 1; dice <= DICE; ++dice) {
        fprintf(stderr, "%s%u:%" PRIu64, dice == 1 ? "" : ",",
                dice, totals.configurations[dice]);
    }
    fputs(", label-symmetry-branches-avoided=", stderr);
    for (dice = 2; dice <= DICE; ++dice) {
        fprintf(stderr, "%s%u:%" PRIu64, dice == 2 ? "" : ",",
                dice, totals.label_symmetry_skips[dice]);
    }
    fputs(", place-projection-prunes=", stderr);
    for (dice = 2; dice <= DICE; ++dice) {
        fprintf(stderr, "%s%u:%" PRIu64, dice == 2 ? "" : ",",
                dice, totals.place_projection_prunes[dice]);
    }
    fputs(", pairwise-prunes=", stderr);
    for (dice = 2; dice <= DICE; ++dice) {
        fprintf(stderr, "%s%u:%" PRIu64, dice == 2 ? "" : ",",
                dice, totals.pairwise_prunes[dice]);
    }
    fputs(", triplet-prunes=", stderr);
    for (dice = 2; dice <= DICE; ++dice) {
        fprintf(stderr, "%s%u:%" PRIu64, dice == 2 ? "" : ",",
                dice, totals.triplet_prunes[dice]);
    }
    fputs(", difference-projection-prunes=", stderr);
    for (dice = 2; dice <= DICE; ++dice) {
        fprintf(stderr, "%s%u:%" PRIu64, dice == 2 ? "" : ",",
                dice, totals.difference_projection_prunes[dice]);
    }
    fputs(", permutation-bound-prunes=", stderr);
    for (dice = 2; dice <= DICE; ++dice) {
        fprintf(stderr, "%s%u:%" PRIu64, dice == 2 ? "" : ",",
                dice, totals.bound_prunes[dice]);
    }
    fputs(", congruence-prunes=", stderr);
    for (dice = 2; dice <= DICE; ++dice) {
        fprintf(stderr, "%s%u:%" PRIu64, dice == 2 ? "" : ",",
                dice, totals.congruence_prunes[dice]);
    }
    fputc('\n', stderr);
}

static bool initialize_worker(struct worker *worker,
                              struct shared_state *shared, unsigned id)
{
    struct search *search;
    size_t scratch_entries =
        (size_t)(FACE_COUNT + 1U) * MAX_PERMUTATIONS;
    unsigned dice;
    unsigned face;

    worker->id = id;
    worker->shared = shared;
    search = calloc(1, sizeof(*search));
    if (search == NULL) {
        return false;
    }
    worker->search = search;
    search->options = shared->options;
    search->shared = shared;
    search->published = &worker->stats;
    for (dice = 0; dice <= DICE; ++dice) {
        atomic_init(&worker->stats.configurations[dice], 0);
        atomic_init(&worker->stats.nodes[dice], 0);
        atomic_init(&worker->stats.label_symmetry_skips[dice], 0);
        atomic_init(&worker->stats.place_projection_prunes[dice], 0);
        atomic_init(&worker->stats.pairwise_prunes[dice], 0);
        atomic_init(&worker->stats.triplet_prunes[dice], 0);
        atomic_init(&worker->stats.difference_projection_prunes[dice], 0);
        atomic_init(&worker->stats.bound_prunes[dice], 0);
        atomic_init(&worker->stats.congruence_prunes[dice], 0);
    }
    if (!initialize_perm_counter(&search->permutations)) {
        return false;
    }
    search->above = calloc(scratch_entries, sizeof(*search->above));
    if (search->above == NULL || !initialize_stages(search)) {
        return false;
    }
    for (face = 0; face < SIDES; ++face) {
        search->word[1][face] = 0;
    }
    if (search->stage[2].possible && !prepare_stage(search, 2)) {
        return false;
    }
    return true;
}

static void destroy_worker(struct worker *worker)
{
    if (worker->search != NULL) {
        free_stages(worker->search);
        free(worker->search);
        worker->search = NULL;
    }
}

static void watch_workers(struct shared_state *shared,
                          const struct worker *workers, double start_time)
{
    double next_progress = start_time + shared->options.progress_seconds;

    while (atomic_load_explicit(&shared->active_workers,
                                memory_order_relaxed) != 0) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
        double now;

        if (interrupt_requested) {
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
        }
        drain_solutions(shared);
        now = monotonic_seconds();
        if (shared->options.progress_seconds != 0 && now >= next_progress) {
            print_totals("progress", shared, workers, start_time);
            next_progress = now + shared->options.progress_seconds;
        }
        nanosleep(&pause, NULL);
    }
    drain_solutions(shared);
}

int main(int argc, char **argv)
{
    struct options options;
    struct shared_state shared;
    struct worker *workers = NULL;
    unsigned requested_threads;
    unsigned initialized_workers = 0;
    unsigned created_threads = 0;
    uint64_t desired_jobs;
    uint64_t desired_prefixes;
    bool stage_two_possible;
    bool synchronization_initialized = false;
    double start_time;
    unsigned i;
    unsigned dice;
    int exit_status = EXIT_SUCCESS;

    if (!parse_options(argc, argv, &options)) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (!install_sigint_handler()) {
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
    desired_jobs = options.jobs != 0
        ? options.jobs
        : (uint64_t)requested_threads * JOBS_PER_WORKER;
    desired_prefixes = desired_jobs > UINT64_MAX / PREFIXES_PER_JOB
        ? UINT64_MAX
        : desired_jobs * PREFIXES_PER_JOB;

    memset(&shared, 0, sizeof(shared));
    shared.options = options;
    shared.prefix_faces = 1;
    shared.prefix_count = prefix_count_for_faces(shared.prefix_faces);
    while (shared.prefix_faces < SIDES &&
           shared.prefix_count < desired_prefixes) {
        ++shared.prefix_faces;
        shared.prefix_count = prefix_count_for_faces(shared.prefix_faces);
    }
    shared.job_count = desired_jobs < shared.prefix_count
        ? desired_jobs
        : shared.prefix_count;
    shared.thread_count = requested_threads;
    if (shared.thread_count > shared.job_count) {
        shared.thread_count = (unsigned)shared.job_count;
    }
    atomic_init(&shared.next_job, 0);
    atomic_init(&shared.jobs_done, 0);
    atomic_init(&shared.solution_total, 0);
    atomic_init(&shared.active_workers, 0);
    atomic_init(&shared.stop, false);
    atomic_init(&shared.internal_error, false);
    if (pthread_mutex_init(&shared.solution_mutex, NULL) != 0) {
        fprintf(stderr, "Unable to initialize solution mutex.\n");
        return EXIT_FAILURE;
    }
    if (pthread_cond_init(&shared.solution_not_full, NULL) != 0) {
        fprintf(stderr, "Unable to initialize solution condition.\n");
        pthread_mutex_destroy(&shared.solution_mutex);
        return EXIT_FAILURE;
    }
    synchronization_initialized = true;

    workers = calloc(shared.thread_count, sizeof(*workers));
    if (workers == NULL) {
        fprintf(stderr, "Unable to allocate worker records.\n");
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }
    for (i = 0; i < shared.thread_count; ++i) {
        if (!initialize_worker(&workers[i], &shared, i)) {
            fprintf(stderr, "Unable to initialize worker %u.\n", i);
            destroy_worker(&workers[i]);
            exit_status = EXIT_FAILURE;
            goto cleanup;
        }
        ++initialized_workers;
    }
    stage_two_possible = workers[0].search->stage[2].possible;

    fprintf(stderr,
            "Searching essentially different %dd%d configurations by "
            "permutation-fair weaving (canonical die labels; %u pthread "
            "workers; %" PRIu64 " jobs over %" PRIu64
            " prefixes, split after %u faces; no column or mirror "
            "restriction)\n",
            DICE, SIDES, shared.thread_count, shared.job_count,
            shared.prefix_count, shared.prefix_faces);
    for (dice = 2; dice <= DICE; ++dice) {
        if (!workers[0].search->stage[dice].possible) {
            fprintf(stderr,
                    "Depth %u cannot be permutation-fair because %u^%u is "
                    "not divisible by %u!.\n",
                    dice, SIDES, dice, dice);
        }
    }

    start_time = monotonic_seconds();
    if (stage_two_possible) {
        for (i = 0; i < shared.thread_count; ++i) {
            int result;

            atomic_fetch_add_explicit(&shared.active_workers, 1,
                                      memory_order_relaxed);
            result = pthread_create(&workers[i].thread, NULL, worker_main,
                                    &workers[i]);
            if (result != 0) {
                fprintf(stderr, "Unable to create worker %u: %s\n", i,
                        strerror(result));
                atomic_fetch_sub_explicit(&shared.active_workers, 1,
                                          memory_order_relaxed);
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
    } else {
        shared.job_count = 0;
    }
    drain_solutions(&shared);

    {
        uint64_t jobs_done = atomic_load_explicit(
            &shared.jobs_done, memory_order_relaxed);
        uint64_t solution_total = atomic_load_explicit(
            &shared.solution_total, memory_order_relaxed);
        bool hit_limit = options.limit != 0 &&
            solution_total >= options.limit;
        bool internal_error = atomic_load_explicit(
            &shared.internal_error, memory_order_relaxed);
        bool incomplete = stage_two_possible && !hit_limit &&
            !interrupt_requested && jobs_done != shared.job_count;
        const char *status;

        if (internal_error) {
            fprintf(stderr, "A worker reported an internal search error.\n");
        }
        if (incomplete && exit_status == EXIT_SUCCESS) {
            fprintf(stderr,
                    "Search stopped before every logical job completed.\n");
        }
        if (internal_error || incomplete) {
            exit_status = EXIT_FAILURE;
        }
        if (interrupt_requested) {
            status = "Search interrupted";
            if (exit_status == EXIT_SUCCESS) {
                exit_status = 128 + SIGINT;
            }
        } else if (exit_status != EXIT_SUCCESS) {
            status = "Search failed";
        } else if (hit_limit) {
            status = "Search stopped at limit";
        } else {
            status = "Search complete";
        }
        print_totals(status, &shared, workers, start_time);
    }

cleanup:
    drain_solutions(&shared);
    for (i = 0; i < initialized_workers; ++i) {
        destroy_worker(&workers[i]);
    }
    free(workers);
    if (synchronization_initialized) {
        pthread_cond_destroy(&shared.solution_not_full);
        pthread_mutex_destroy(&shared.solution_mutex);
    }
    return exit_status;
}
