#define _POSIX_C_SOURCE 200809L

/*
 * Exhaustive Go First Dice search by residual subsequence counts.
 *
 * For every ordered tuple q of distinct dice, residual[q] is the number of
 * q-shaped rolls that the unknown suffix of a fair configuration must have.
 * Initially residual[q] = SIDES^|q| / |q|!.  Appending owner x to the known
 * prefix performs the exact deconcatenation update
 *
 *     residual[xq] -= residual[q].
 *
 * A negative result proves that no real suffix can complete the prefix.
 * Recursion updates one shared state in place and restores it before
 * returning; the search performs no allocation.
 *
 * Pthreads are confined to the driver around that recursion.  The main
 * thread builds immutable canonical prefix jobs, and every worker owns one
 * private mutable search state.  Workers claim jobs atomically but take no
 * locks and allocate no memory while exploring nonsolutions.  New pruning
 * belongs in try_owner(), so prefix construction, replay, and recursive
 * search all continue to use the same transition rules.
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
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

#ifndef RESIDUAL_RIGHT_END_CHECK
#define RESIDUAL_RIGHT_END_CHECK 1
#endif

#ifndef RESIDUAL_TWO_END_SEARCH
#define RESIDUAL_TWO_END_SEARCH 1
#endif

#define FACE_COUNT (DICE * SIDES)
#define DEFAULT_PRINT_LIMIT UINT64_C(10)
#define DEFAULT_JOBS_PER_THREAD UINT64_C(8)
#define PUBLISH_NODE_INTERVAL UINT64_C(1048576)
#define STOP_POLL_INTERVAL UINT64_C(4096)
#define PREFIX_RIGHT_FLAG UINT8_C(0x80)

#if DICE == 2
#define PERM_STATE_COUNT 5U
#define OWNER_EDGE_COUNT 2U
#elif DICE == 3
#define PERM_STATE_COUNT 16U
#define OWNER_EDGE_COUNT 5U
#elif DICE == 4
#define PERM_STATE_COUNT 65U
#define OWNER_EDGE_COUNT 16U
#elif DICE == 5
#define PERM_STATE_COUNT 326U
#define OWNER_EDGE_COUNT 65U
#elif DICE == 6
#define PERM_STATE_COUNT 1957U
#define OWNER_EDGE_COUNT 326U
#else
#error "residual_search supports DICE values from 2 through 6"
#endif

_Static_assert(SIDES > 0, "SIDES must be positive");
_Static_assert(SIDES <= UINT16_MAX, "remaining face counts must fit uint16_t");
_Static_assert(FACE_COUNT <= UINT16_MAX, "prefix depth must fit uint16_t");
_Static_assert(PERM_STATE_COUNT <= UINT16_MAX,
               "permutation state indices must fit uint16_t");
struct options {
    uint64_t limit;
    uint64_t node_limit;
    uint64_t print_limit;
    uint64_t jobs;
    uint64_t seed;
    unsigned threads;
    bool quiet;
    bool threads_given;
    bool random_order;
    bool seed_given;
};

struct perm_state {
    uint64_t key;
    unsigned mask;
    unsigned length;
};

struct edge {
    uint16_t source;
    uint16_t destination;
};

struct shared_state;
struct published_stats;

struct search {
    struct perm_state state[PERM_STATE_COUNT];
    struct edge prepend[DICE][OWNER_EDGE_COUNT];
    struct edge append[DICE][OWNER_EDGE_COUNT];
    size_t length_begin[DICE + 2U];
    uint64_t target[DICE + 1U];
    uint64_t residual[PERM_STATE_COUNT];
    uint16_t remaining[DICE];
    char encoding[FACE_COUNT + 1U];
    uint64_t nodes;
    uint64_t negative_prunes;
    uint64_t right_end_prunes;
    uint64_t left_end_prunes;
    uint64_t configurations;
    bool possible;
    bool internal_error;
    uint64_t node_base;
    struct shared_state *shared;
    struct published_stats *published;
};

struct prefix_list {
    uint8_t *owners;
    uint64_t count;
    uint64_t capacity;
    unsigned depth;
};

struct published_stats {
    atomic_uint_fast64_t nodes;
    atomic_uint_fast64_t negative_prunes;
    atomic_uint_fast64_t right_end_prunes;
    atomic_uint_fast64_t left_end_prunes;
};

struct solution {
    struct solution *next;
    uint64_t number;
    char encoding[FACE_COUNT + 1U];
};

struct shared_state {
    struct options options;
    struct prefix_list prefixes;
    uint64_t *job_order;
    uint64_t job_count;
    uint64_t setup_nodes;
    uint64_t setup_negative_prunes;
    uint64_t setup_right_end_prunes;
    uint64_t setup_left_end_prunes;
    atomic_uint_fast64_t next_job;
    atomic_uint_fast64_t jobs_done;
    atomic_uint_fast64_t configurations;
    atomic_uint active_workers;
    atomic_bool stop;
    atomic_bool internal_error;
    atomic_bool node_limit_reached;
    pthread_mutex_t solution_mutex;
    pthread_mutex_t event_mutex;
    pthread_cond_t event_condition;
    struct solution *solution_head;
    struct solution *solution_tail;
};

struct worker {
    pthread_t thread;
    struct shared_state *shared;
    struct search search;
    struct published_stats stats;
};

struct totals {
    uint64_t nodes;
    uint64_t negative_prunes;
    uint64_t right_end_prunes;
    uint64_t left_end_prunes;
    uint64_t configurations;
};

enum owner_result {
    OWNER_ACCEPTED,
    OWNER_RIGHT_END_IMPOSSIBLE,
    OWNER_LEFT_END_IMPOSSIBLE,
};

enum search_end {
    SEARCH_LEFT,
    SEARCH_RIGHT,
};

struct end_plan {
    enum search_end end;
    unsigned owner_mask;
    unsigned rejected_owners;
};

static volatile sig_atomic_t interrupt_requested;

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [OPTIONS]\n\n"
            "Exhaustively search canonical %dd%d owner strings using exact "
            "residual roll counts.\n\n"
            "  -t, --threads N     worker threads; default is online CPUs\n"
            "  -j, --jobs N        logical prefix jobs; default is 8 per worker\n"
            "  -n, --limit N       stop after N configurations\n"
            "      --node-limit N  stop after visiting N search nodes\n"
            "      --print-limit N print at most N configurations\n"
            "      --all-solutions print every configuration\n"
            "      --random-order  shuffle logical jobs before searching\n"
            "      --seed N        random-order seed; requires --random-order\n"
            "  -q, --quiet         suppress configuration output\n"
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
    int argument;

    *options = (struct options){
        .print_limit = DEFAULT_PRINT_LIMIT,
    };
    for (argument = 1; argument < argc; ++argument) {
        const char *option = argv[argument];

        if (strcmp(option, "-q") == 0 || strcmp(option, "--quiet") == 0) {
            options->quiet = true;
        } else if (strcmp(option, "--all-solutions") == 0) {
            options->print_limit = UINT64_MAX;
        } else if (strcmp(option, "-t") == 0 ||
                   strcmp(option, "--threads") == 0) {
            uint64_t threads;

            if (++argument == argc ||
                !parse_uint64(argv[argument], &threads) || threads == 0 ||
                threads > UINT_MAX) {
                return false;
            }
            options->threads = (unsigned)threads;
            options->threads_given = true;
        } else if (strcmp(option, "-j") == 0 ||
                   strcmp(option, "--jobs") == 0) {
            if (++argument == argc ||
                !parse_uint64(argv[argument], &options->jobs) ||
                options->jobs == 0) {
                return false;
            }
        } else if (strcmp(option, "-n") == 0 ||
                   strcmp(option, "--limit") == 0) {
            if (++argument == argc ||
                !parse_uint64(argv[argument], &options->limit)) {
                return false;
            }
        } else if (strcmp(option, "--random-order") == 0) {
            options->random_order = true;
        } else if (strcmp(option, "--seed") == 0) {
            if (++argument == argc ||
                !parse_uint64(argv[argument], &options->seed)) {
                return false;
            }
            options->seed_given = true;
        } else if (strcmp(option, "--print-limit") == 0) {
            if (++argument == argc ||
                !parse_uint64(argv[argument], &options->print_limit)) {
                return false;
            }
        } else if (strcmp(option, "--node-limit") == 0) {
            if (++argument == argc ||
                !parse_uint64(argv[argument], &options->node_limit)) {
                return false;
            }
        } else if (strcmp(option, "-h") == 0 ||
                   strcmp(option, "--help") == 0) {
            usage(stdout, argv[0]);
            exit(EXIT_SUCCESS);
        } else {
            return false;
        }
    }
    if (options->seed_given && !options->random_order) {
        fputs("--seed requires --random-order.\n", stderr);
        return false;
    }
    return true;
}

static bool integer_power(uint64_t base, unsigned exponent, uint64_t *result)
{
    uint64_t value = 1;

    while (exponent-- > 0) {
        if (base != 0 && value > UINT64_MAX / base) {
            return false;
        }
        value *= base;
    }
    *result = value;
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

static unsigned online_cpu_count(void)
{
    long count = sysconf(_SC_NPROCESSORS_ONLN);

    return count > 0 && (unsigned long)count <= UINT_MAX
        ? (unsigned)count : 1U;
}

static uint64_t default_random_seed(void)
{
    struct timespec realtime = {0};

    (void)clock_gettime(CLOCK_REALTIME, &realtime);
    return (uint64_t)realtime.tv_sec ^
        ((uint64_t)realtime.tv_nsec << 32U) ^ (uint64_t)getpid();
}

static uint64_t splitmix64(uint64_t *state)
{
    uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));

    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

static void handle_sigint(int signal_number)
{
    (void)signal_number;
    interrupt_requested = 1;
}

static bool install_signal_handler(void)
{
    struct sigaction action = {0};

    action.sa_handler = handle_sigint;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0) {
        fprintf(stderr, "Unable to install SIGINT handler: %s\n",
                strerror(errno));
        return false;
    }
    return true;
}

static bool generate_states(struct search *search, unsigned length,
                            unsigned depth, unsigned mask, uint64_t key,
                            size_t *next)
{
    unsigned owner;

    if (depth == length) {
        if (*next >= PERM_STATE_COUNT) {
            return false;
        }
        search->state[*next] = (struct perm_state){
            .key = key,
            .mask = mask,
            .length = length,
        };
        ++*next;
        return true;
    }
    for (owner = 0; owner < DICE; ++owner) {
        if ((mask & (1U << owner)) == 0 &&
            !generate_states(search, length, depth + 1U,
                             mask | (1U << owner),
                             key | ((uint64_t)(owner + 1U) << (4U * depth)),
                             next)) {
            return false;
        }
    }
    return true;
}

static int find_state(const struct search *search, unsigned length,
                      uint64_t key)
{
    size_t index;

    for (index = search->length_begin[length];
         index < search->length_begin[length + 1U]; ++index) {
        if (search->state[index].key == key) {
            return (int)index;
        }
    }
    return -1;
}

static uint64_t prepend_owner_key(uint64_t key, unsigned owner)
{
    return (key << 4U) | (uint64_t)(owner + 1U);
}

static uint64_t append_owner_key(const struct perm_state *state,
                                 unsigned owner)
{
    return state->key |
        ((uint64_t)(owner + 1U) << (4U * state->length));
}

static bool initialize_states_and_edges(struct search *search)
{
    size_t next = 0;
    unsigned length;
    unsigned owner;

    for (length = 0; length <= DICE; ++length) {
        search->length_begin[length] = next;
        if (!generate_states(search, length, 0, 0, 0, &next)) {
            return false;
        }
    }
    search->length_begin[DICE + 1U] = next;
    if (next != PERM_STATE_COUNT) {
        return false;
    }

    for (owner = 0; owner < DICE; ++owner) {
        unsigned edge_count = 0;
        size_t source = PERM_STATE_COUNT;

        /* Highest-order residual counts are usually the first to go negative. */
        while (source-- > 0) {
            const struct perm_state *state = &search->state[source];
            int prepended;
            int appended;

            if (state->length == DICE ||
                (state->mask & (1U << owner)) != 0) {
                continue;
            }
            if (edge_count >= OWNER_EDGE_COUNT) {
                return false;
            }
            prepended = find_state(
                search, state->length + 1U,
                prepend_owner_key(state->key, owner));
            appended = find_state(
                search, state->length + 1U,
                append_owner_key(state, owner));
            if (prepended < 0 || appended < 0) {
                return false;
            }
            search->prepend[owner][edge_count] = (struct edge){
                .source = (uint16_t)source,
                .destination = (uint16_t)prepended,
            };
            search->append[owner][edge_count] = (struct edge){
                .source = (uint16_t)source,
                .destination = (uint16_t)appended,
            };
            ++edge_count;
        }
        if (edge_count != OWNER_EDGE_COUNT) {
            return false;
        }
    }
    return true;
}

static bool initialize_targets(struct search *search)
{
    uint64_t factorial = 1;
    unsigned length;
    size_t state;

    search->possible = true;
    search->target[0] = 1;
    for (length = 1; length <= DICE; ++length) {
        uint64_t outcomes;

        factorial *= length;
        if (!integer_power(SIDES, length, &outcomes)) {
            fprintf(stderr, "%d^%u exceeds the 64-bit counting range.\n",
                    SIDES, length);
            return false;
        }
        if (outcomes % factorial != 0) {
            fprintf(stderr,
                    "No solutions: %d^%u is not divisible by %" PRIu64
                    " (%u!).\n",
                    SIDES, length, factorial, length);
            search->possible = false;
            return true;
        }
        search->target[length] = outcomes / factorial;
    }
    for (state = 0; state < PERM_STATE_COUNT; ++state) {
        search->residual[state] =
            search->target[search->state[state].length];
    }
    return true;
}

static void subtract_prefix_owner(struct search *search, unsigned owner)
{
    unsigned edge;

    for (edge = 0; edge < OWNER_EDGE_COUNT; ++edge) {
        const struct edge *transition = &search->prepend[owner][edge];

        search->residual[transition->destination] -=
            search->residual[transition->source];
    }
}

static void restore_prefix_owner(struct search *search, unsigned owner)
{
    unsigned edge;

    for (edge = 0; edge < OWNER_EDGE_COUNT; ++edge) {
        const struct edge *transition = &search->prepend[owner][edge];

        search->residual[transition->destination] +=
            search->residual[transition->source];
    }
}

#if RESIDUAL_TWO_END_SEARCH
static void subtract_suffix_owner(struct search *search, unsigned owner)
{
    unsigned edge;

    for (edge = 0; edge < OWNER_EDGE_COUNT; ++edge) {
        const struct edge *transition = &search->append[owner][edge];

        search->residual[transition->destination] -=
            search->residual[transition->source];
    }
}

static void restore_suffix_owner(struct search *search, unsigned owner)
{
    unsigned edge;

    for (edge = 0; edge < OWNER_EDGE_COUNT; ++edge) {
        const struct edge *transition = &search->append[owner][edge];

        search->residual[transition->destination] +=
            search->residual[transition->source];
    }
}
#endif

static unsigned feasible_owner_mask(
    const struct search *search,
    const struct edge (*edges)[OWNER_EDGE_COUNT], unsigned owner_limit)
{
    unsigned mask = 0;
    unsigned owner;

    for (owner = 0; owner < owner_limit; ++owner) {
        unsigned edge;

        if (search->remaining[owner] == 0) {
            continue;
        }
        for (edge = 0; edge < OWNER_EDGE_COUNT; ++edge) {
            const struct edge *transition = &edges[owner][edge];

            if (search->residual[transition->destination] <
                search->residual[transition->source]) {
                break;
            }
        }
        if (edge == OWNER_EDGE_COUNT) {
            mask |= 1U << owner;
        }
    }
    return mask;
}

static bool has_feasible_owner(
    const struct search *search,
    const struct edge (*edges)[OWNER_EDGE_COUNT], unsigned owner_limit)
{
    unsigned owner;

    for (owner = 0; owner < owner_limit; ++owner) {
        unsigned edge;

        if (search->remaining[owner] == 0) {
            continue;
        }
        for (edge = 0; edge < OWNER_EDGE_COUNT; ++edge) {
            const struct edge *transition = &edges[owner][edge];

            if (search->residual[transition->destination] <
                search->residual[transition->source]) {
                break;
            }
        }
        if (edge == OWNER_EDGE_COUNT) {
            return true;
        }
    }
    return false;
}

static unsigned available_owner_mask(const struct search *search,
                                     unsigned owner_limit)
{
    unsigned mask = 0;
    unsigned owner;

    for (owner = 0; owner < owner_limit; ++owner) {
        if (search->remaining[owner] != 0) {
            mask |= 1U << owner;
        }
    }
    return mask;
}

static unsigned owner_mask_count(unsigned mask)
{
    unsigned count = 0;

    while (mask != 0) {
        mask &= mask - 1U;
        ++count;
    }
    return count;
}

/*
 * Every nonempty realizable residual word has some final owner.  If that
 * owner is x, removing its final occurrence performs the right-handed
 * deconcatenation
 *
 *     residual[qx] -= residual[q].
 *
 * Consequently x can only be the final owner when every such subtraction
 * is nonnegative.  We do not modify the residual here: finding any possible
 * final owner is enough to keep searching from the left.
 */
#if RESIDUAL_RIGHT_END_CHECK
static bool has_feasible_last_owner(const struct search *search)
{
    return has_feasible_owner(search, search->append, DICE);
}
#endif

#if RESIDUAL_TWO_END_SEARCH
static bool has_feasible_first_owner(const struct search *search)
{
    return has_feasible_owner(search, search->prepend, DICE);
}

static struct end_plan plan_search_end(const struct search *search,
                                       unsigned used_labels)
{
    unsigned owner_limit = used_labels < DICE ? used_labels + 1U : DICE;
    unsigned available = available_owner_mask(search, owner_limit);
    unsigned left_mask = feasible_owner_mask(search, search->prepend,
                                             owner_limit);
    struct end_plan plan = {
        .end = SEARCH_LEFT,
        .owner_mask = left_mask,
    };

    if (used_labels < DICE) {
        plan.rejected_owners = owner_mask_count(available) -
            owner_mask_count(left_mask);
        return plan;
    }
    {
        unsigned right_mask = feasible_owner_mask(search, search->append,
                                                  DICE);

        if (owner_mask_count(right_mask) < owner_mask_count(left_mask)) {
            plan.end = SEARCH_RIGHT;
            plan.owner_mask = right_mask;
        }
    }
    plan.rejected_owners = owner_mask_count(available) -
        owner_mask_count(plan.owner_mask);
    return plan;
}
#else
static struct end_plan plan_search_end(const struct search *search,
                                       unsigned used_labels)
{
    unsigned owner_limit = used_labels < DICE ? used_labels + 1U : DICE;
    unsigned available = available_owner_mask(search, owner_limit);
    unsigned feasible = feasible_owner_mask(search, search->prepend,
                                            owner_limit);

    return (struct end_plan){
        .end = SEARCH_LEFT,
        .owner_mask = feasible,
        .rejected_owners = owner_mask_count(available) -
            owner_mask_count(feasible),
    };
}
#endif

static bool verify_configuration(const struct search *search)
{
    uint64_t ways[PERM_STATE_COUNT] = {0};
    unsigned face;
    size_t state;

    ways[0] = 1;
    for (face = 0; face < FACE_COUNT; ++face) {
        unsigned owner = (unsigned)(search->encoding[face] - 'a');
        unsigned edge;

        if (owner >= DICE) {
            return false;
        }
        for (edge = 0; edge < OWNER_EDGE_COUNT; ++edge) {
            const struct edge *transition = &search->append[owner][edge];

            ways[transition->destination] += ways[transition->source];
        }
    }
    for (state = 0; state < PERM_STATE_COUNT; ++state) {
        if (ways[state] != search->target[search->state[state].length] ||
            (state != 0 && search->residual[state] != 0)) {
            return false;
        }
    }
    return true;
}

static enum owner_result try_owner(struct search *search, unsigned owner,
                                   unsigned assigned, unsigned position,
                                   enum search_end end)
{
    --search->remaining[owner];
    search->encoding[position] = (char)('a' + owner);
#if RESIDUAL_TWO_END_SEARCH
    if (end == SEARCH_RIGHT) {
        subtract_suffix_owner(search, owner);
        if (assigned + 1U < FACE_COUNT &&
            !has_feasible_first_owner(search)) {
            restore_suffix_owner(search, owner);
            ++search->remaining[owner];
            return OWNER_LEFT_END_IMPOSSIBLE;
        }
        return OWNER_ACCEPTED;
    }
#else
    (void)end;
#endif
    subtract_prefix_owner(search, owner);
#if RESIDUAL_RIGHT_END_CHECK
    if (assigned + 1U < FACE_COUNT && !has_feasible_last_owner(search)) {
        restore_prefix_owner(search, owner);
        ++search->remaining[owner];
        return OWNER_RIGHT_END_IMPOSSIBLE;
    }
#endif
    return OWNER_ACCEPTED;
}

static void undo_owner(struct search *search, unsigned owner,
                       enum search_end end)
{
#if RESIDUAL_TWO_END_SEARCH
    if (end == SEARCH_RIGHT) {
        restore_suffix_owner(search, owner);
        ++search->remaining[owner];
        return;
    }
#else
    (void)end;
#endif
    restore_prefix_owner(search, owner);
    ++search->remaining[owner];
}

static void publish_stats(const struct search *search)
{
    if (search->published == NULL) {
        return;
    }
    atomic_store_explicit(&search->published->nodes, search->nodes,
                          memory_order_relaxed);
    atomic_store_explicit(&search->published->negative_prunes,
                          search->negative_prunes, memory_order_relaxed);
    atomic_store_explicit(&search->published->right_end_prunes,
                          search->right_end_prunes, memory_order_relaxed);
    atomic_store_explicit(&search->published->left_end_prunes,
                          search->left_end_prunes, memory_order_relaxed);
}

static bool search_should_stop(const struct search *search)
{
    return search->internal_error ||
        (search->shared != NULL &&
         atomic_load_explicit(&search->shared->stop, memory_order_relaxed));
}

static bool reserve_solution_number(struct shared_state *shared,
                                    uint64_t *number)
{
    uint_fast64_t current = atomic_load_explicit(
        &shared->configurations, memory_order_relaxed);

    for (;;) {
        if (shared->options.limit != 0 && current >= shared->options.limit) {
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &shared->configurations, &current, current + 1U,
                memory_order_relaxed, memory_order_relaxed)) {
            *number = (uint64_t)current + 1U;
            return true;
        }
    }
}

static bool record_configuration(struct search *search)
{
    struct shared_state *shared = search->shared;
    struct solution *solution = NULL;
    uint64_t number;

    if (!verify_configuration(search)) {
        search->internal_error = true;
        atomic_store_explicit(&shared->internal_error, true,
                              memory_order_relaxed);
        atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
        return false;
    }
    if (!reserve_solution_number(shared, &number)) {
        return false;
    }
    ++search->configurations;
    if (!shared->options.quiet && number <= shared->options.print_limit) {
        solution = malloc(sizeof(*solution));
        if (solution == NULL) {
            search->internal_error = true;
            atomic_store_explicit(&shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
            return false;
        }
        solution->next = NULL;
        solution->number = number;
        memcpy(solution->encoding, search->encoding,
               sizeof(solution->encoding));
        pthread_mutex_lock(&shared->solution_mutex);
        if (shared->solution_tail != NULL) {
            shared->solution_tail->next = solution;
        } else {
            shared->solution_head = solution;
        }
        shared->solution_tail = solution;
        pthread_mutex_unlock(&shared->solution_mutex);
        pthread_mutex_lock(&shared->event_mutex);
        pthread_cond_signal(&shared->event_condition);
        pthread_mutex_unlock(&shared->event_mutex);
    }
    if (shared->options.limit != 0 && number >= shared->options.limit) {
        atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
    }
    return true;
}

static void search_configurations(struct search *search, unsigned left_depth,
                                  unsigned right_depth,
                                  unsigned used_labels)
{
    unsigned assigned = left_depth + right_depth;
    struct end_plan plan;
    enum search_end end;
    unsigned position;
    unsigned owner;

    if (search->internal_error) {
        return;
    }
    if ((search->nodes & (STOP_POLL_INTERVAL - 1U)) == 0 &&
        atomic_load_explicit(&search->shared->stop, memory_order_relaxed)) {
        return;
    }
    if (search->shared->options.node_limit != 0 &&
        search->node_base + search->nodes >=
            search->shared->options.node_limit) {
        atomic_store_explicit(&search->shared->node_limit_reached, true,
                              memory_order_relaxed);
        atomic_store_explicit(&search->shared->stop, true,
                              memory_order_relaxed);
        return;
    }
    ++search->nodes;
    if ((search->nodes & (PUBLISH_NODE_INTERVAL - 1U)) == 0) {
        publish_stats(search);
    }
    if (assigned == FACE_COUNT) {
        (void)record_configuration(search);
        return;
    }

    plan = plan_search_end(search, used_labels);
    end = plan.end;
    search->negative_prunes += plan.rejected_owners;
    position = end == SEARCH_LEFT ? left_depth :
        FACE_COUNT - 1U - right_depth;

    for (owner = 0; owner < DICE; ++owner) {
        bool new_label = end == SEARCH_LEFT && owner == used_labels;

        if ((plan.owner_mask & (1U << owner)) == 0) {
            continue;
        }
        switch (try_owner(search, owner, assigned, position, end)) {
        case OWNER_RIGHT_END_IMPOSSIBLE:
            ++search->right_end_prunes;
            break;
        case OWNER_LEFT_END_IMPOSSIBLE:
            ++search->left_end_prunes;
            break;
        case OWNER_ACCEPTED:
            search_configurations(
                search, left_depth + (end == SEARCH_LEFT ? 1U : 0U),
                right_depth + (end == SEARCH_RIGHT ? 1U : 0U),
                used_labels + (new_label ? 1U : 0U));
            undo_owner(search, owner, end);
            break;
        }
        if (search->internal_error) {
            return;
        }
    }
}

static bool append_prefix(struct prefix_list *prefixes, const uint8_t *steps)
{
    if (prefixes->count == prefixes->capacity) {
        uint64_t capacity = prefixes->capacity == 0
            ? UINT64_C(64) : prefixes->capacity * 2U;
        uint8_t *owners;

        if (capacity < prefixes->capacity ||
            capacity > SIZE_MAX / prefixes->depth) {
            return false;
        }
        owners = realloc(prefixes->owners,
                         (size_t)capacity * prefixes->depth);
        if (owners == NULL) {
            return false;
        }
        prefixes->owners = owners;
        prefixes->capacity = capacity;
    }
    memcpy(prefixes->owners + (size_t)prefixes->count * prefixes->depth,
           steps, prefixes->depth);
    ++prefixes->count;
    return true;
}

static bool collect_prefixes(struct search *search,
                             struct prefix_list *prefixes, uint8_t *steps,
                             unsigned left_depth, unsigned right_depth,
                             unsigned used_labels)
{
    unsigned assigned = left_depth + right_depth;
    struct end_plan plan;
    enum search_end end;
    unsigned position;
    unsigned owner;

    if (interrupt_requested) {
        return false;
    }
    if (assigned == prefixes->depth) {
        return append_prefix(prefixes, steps);
    }
    ++search->nodes;
    plan = plan_search_end(search, used_labels);
    end = plan.end;
    search->negative_prunes += plan.rejected_owners;
    position = end == SEARCH_LEFT ? left_depth :
        FACE_COUNT - 1U - right_depth;
    for (owner = 0; owner < DICE; ++owner) {
        bool new_label = end == SEARCH_LEFT && owner == used_labels;

        if ((plan.owner_mask & (1U << owner)) == 0) {
            continue;
        }
        steps[assigned] = (uint8_t)owner |
            (end == SEARCH_RIGHT ? PREFIX_RIGHT_FLAG : 0U);
        switch (try_owner(search, owner, assigned, position, end)) {
        case OWNER_RIGHT_END_IMPOSSIBLE:
            ++search->right_end_prunes;
            break;
        case OWNER_LEFT_END_IMPOSSIBLE:
            ++search->left_end_prunes;
            break;
        case OWNER_ACCEPTED:
            if (!collect_prefixes(
                    search, prefixes, steps,
                    left_depth + (end == SEARCH_LEFT ? 1U : 0U),
                    right_depth + (end == SEARCH_RIGHT ? 1U : 0U),
                    used_labels + (new_label ? 1U : 0U))) {
                undo_owner(search, owner, end);
                return false;
            }
            undo_owner(search, owner, end);
            break;
        }
    }
    return true;
}

static bool build_prefixes(struct search *prototype,
                           struct prefix_list *prefixes,
                           uint64_t desired_jobs)
{
    uint8_t steps[FACE_COUNT];
    unsigned depth;

    for (depth = 1; depth <= FACE_COUNT; ++depth) {
        free(prefixes->owners);
        *prefixes = (struct prefix_list){.depth = depth};
        prototype->nodes = 0;
        prototype->negative_prunes = 0;
        prototype->right_end_prunes = 0;
        prototype->left_end_prunes = 0;
        if (!collect_prefixes(prototype, prefixes, steps, 0, 0, 0)) {
            return false;
        }
        if (prefixes->count == 0 || prefixes->count >= desired_jobs ||
            depth == FACE_COUNT) {
            return true;
        }
    }
    return false;
}

static void shuffle_jobs(struct shared_state *shared)
{
    uint64_t state = shared->options.seed;
    uint64_t index;

    for (index = shared->job_count; index > 1; --index) {
        uint64_t other = splitmix64(&state) % index;
        uint64_t temporary = shared->job_order[index - 1U];

        shared->job_order[index - 1U] = shared->job_order[other];
        shared->job_order[other] = temporary;
    }
}

static void job_prefix_range(const struct shared_state *shared, uint64_t job,
                             uint64_t *begin, uint64_t *end)
{
    uint64_t base = shared->prefixes.count / shared->job_count;
    uint64_t extra = shared->prefixes.count % shared->job_count;

    *begin = job * base + (job < extra ? job : extra);
    *end = *begin + base + (job < extra ? 1U : 0U);
}

static bool run_prefix(struct search *search, uint64_t prefix_index)
{
    const struct prefix_list *prefixes = &search->shared->prefixes;
    const uint8_t *prefix = prefixes->owners +
        (size_t)prefix_index * prefixes->depth;
    unsigned used_labels = 0;
    unsigned left_depth = 0;
    unsigned right_depth = 0;
    unsigned applied = 0;
    unsigned depth;

    for (depth = 0; depth < prefixes->depth; ++depth) {
        uint8_t step = prefix[depth];
        struct end_plan plan = plan_search_end(search, used_labels);
        enum search_end end = (step & PREFIX_RIGHT_FLAG) != 0
            ? SEARCH_RIGHT : SEARCH_LEFT;
        unsigned owner = step & ~PREFIX_RIGHT_FLAG;
        unsigned position = end == SEARCH_LEFT ? left_depth :
            FACE_COUNT - 1U - right_depth;

        if (owner >= DICE || search->remaining[owner] == 0 ||
            end != plan.end || (plan.owner_mask & (1U << owner)) == 0 ||
            try_owner(search, owner, depth, position, end) != OWNER_ACCEPTED) {
            search->internal_error = true;
            atomic_store_explicit(&search->shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&search->shared->stop, true,
                                  memory_order_relaxed);
            break;
        }
        ++applied;
        left_depth += end == SEARCH_LEFT;
        right_depth += end == SEARCH_RIGHT;
        if (end == SEARCH_LEFT && owner == used_labels) {
            ++used_labels;
        }
    }
    if (!search->internal_error) {
        search_configurations(search, left_depth, right_depth, used_labels);
    }
    while (applied > 0) {
        uint8_t step;
        enum search_end end;

        --applied;
        step = prefix[applied];
        end = (step & PREFIX_RIGHT_FLAG) != 0
            ? SEARCH_RIGHT : SEARCH_LEFT;
        undo_owner(search, step & ~PREFIX_RIGHT_FLAG, end);
    }
    return !search_should_stop(search);
}

static bool run_job(struct worker *worker, uint64_t job)
{
    uint64_t begin;
    uint64_t end;
    uint64_t prefix;

    job_prefix_range(worker->shared, job, &begin, &end);
    for (prefix = begin; prefix < end; ++prefix) {
        if (!run_prefix(&worker->search, prefix)) {
            return false;
        }
    }
    return true;
}

static void *worker_main(void *argument)
{
    struct worker *worker = argument;
    struct shared_state *shared = worker->shared;

    for (;;) {
        uint64_t slot;
        uint64_t job;

        if (atomic_load_explicit(&shared->stop, memory_order_relaxed)) {
            break;
        }
        slot = atomic_fetch_add_explicit(&shared->next_job, 1,
                                         memory_order_relaxed);
        if (slot >= shared->job_count) {
            break;
        }
        job = shared->job_order[slot];
        if (!run_job(worker, job)) {
            break;
        }
        atomic_fetch_add_explicit(&shared->jobs_done, 1,
                                  memory_order_relaxed);
        publish_stats(&worker->search);
    }
    publish_stats(&worker->search);
    pthread_mutex_lock(&shared->event_mutex);
    atomic_fetch_sub_explicit(&shared->active_workers, 1,
                              memory_order_release);
    pthread_cond_signal(&shared->event_condition);
    pthread_mutex_unlock(&shared->event_mutex);
    return NULL;
}

static struct totals collect_totals(const struct shared_state *shared,
                                    const struct worker *workers,
                                    unsigned worker_count)
{
    struct totals totals = {
        .nodes = shared->setup_nodes,
        .negative_prunes = shared->setup_negative_prunes,
        .right_end_prunes = shared->setup_right_end_prunes,
        .left_end_prunes = shared->setup_left_end_prunes,
        .configurations = atomic_load_explicit(
            &shared->configurations, memory_order_relaxed),
    };
    unsigned worker;

    for (worker = 0; worker < worker_count; ++worker) {
        totals.nodes += atomic_load_explicit(&workers[worker].stats.nodes,
                                             memory_order_relaxed);
        totals.negative_prunes += atomic_load_explicit(
            &workers[worker].stats.negative_prunes, memory_order_relaxed);
        totals.right_end_prunes += atomic_load_explicit(
            &workers[worker].stats.right_end_prunes, memory_order_relaxed);
        totals.left_end_prunes += atomic_load_explicit(
            &workers[worker].stats.left_end_prunes, memory_order_relaxed);
    }
    return totals;
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

        printf("permutation-fair #%" PRIu64 " depth=%dd%d encoding=%s\n",
               solution->number, DICE, SIDES, solution->encoding);
        free(solution);
        solution = next;
    }
    fflush(stdout);
}

static void print_progress(const struct shared_state *shared,
                           const struct worker *workers,
                           unsigned worker_count, double start_time,
                           bool terminal)
{
    struct totals totals = collect_totals(shared, workers, worker_count);
    uint64_t jobs_done = atomic_load_explicit(&shared->jobs_done,
                                              memory_order_relaxed);
    unsigned active = atomic_load_explicit(&shared->active_workers,
                                           memory_order_relaxed);

    fprintf(stderr,
            "%sprogress: %.1fs, workers=%u, jobs=%" PRIu64 "/%" PRIu64
            ", nodes=%" PRIu64 ", negative-prunes=%" PRIu64
            ", right-end-prunes=%" PRIu64 ", left-end-prunes=%" PRIu64
            ", permutation-fair=%" PRIu64 "%s",
            terminal ? "\r" : "", monotonic_seconds() - start_time, active,
            jobs_done, shared->job_count, totals.nodes,
            totals.negative_prunes, totals.right_end_prunes,
            totals.left_end_prunes, totals.configurations,
            terminal ? "\033[K" : "\n");
    fflush(stderr);
}

static void watch_workers(struct shared_state *shared,
                          const struct worker *workers,
                          unsigned worker_count, double start_time)
{
    const bool terminal = isatty(STDERR_FILENO) != 0;
    double next_status = monotonic_seconds() + 1.0;
    bool printed_status = false;

    pthread_mutex_lock(&shared->event_mutex);
    while (atomic_load_explicit(&shared->active_workers,
                                memory_order_acquire) != 0) {
        struct timespec deadline;
        double now;
        double wait_seconds;

        pthread_mutex_unlock(&shared->event_mutex);
        if (interrupt_requested) {
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
        }
        drain_solutions(shared);
        now = monotonic_seconds();
        if (now >= next_status) {
            print_progress(shared, workers, worker_count, start_time,
                           terminal);
            printed_status = true;
            next_status = now + 1.0;
        }
        wait_seconds = next_status - monotonic_seconds();
        if (wait_seconds < 0.001) {
            wait_seconds = 0.001;
        }
        (void)clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)wait_seconds;
        deadline.tv_nsec += (long)((wait_seconds - (time_t)wait_seconds) *
                                   1000000000.0);
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&shared->event_mutex);
        if (atomic_load_explicit(&shared->active_workers,
                                 memory_order_acquire) != 0) {
            (void)pthread_cond_timedwait(&shared->event_condition,
                                         &shared->event_mutex, &deadline);
        }
    }
    pthread_mutex_unlock(&shared->event_mutex);
    drain_solutions(shared);
    if (terminal && printed_status) {
        fputc('\n', stderr);
    }
}

int main(int argc, char **argv)
{
    struct options options;
    struct search prototype = {0};
    struct shared_state shared = {0};
    struct worker *workers = NULL;
    uint64_t desired_jobs;
    unsigned worker_count = 0;
    unsigned created_workers = 0;
    double start_time;
    unsigned owner;
    unsigned worker;
    int exit_status = EXIT_SUCCESS;

    if (!parse_options(argc, argv, &options)) {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (options.threads == 0) {
        options.threads = online_cpu_count();
    }
    if (options.node_limit != 0 && options.threads != 1U) {
        if (options.threads_given) {
            fputs("--node-limit currently requires --threads 1.\n", stderr);
            return EXIT_FAILURE;
        }
        options.threads = 1U;
    }
    if (options.node_limit != 0 && options.jobs > 1U) {
        fputs("--node-limit requires --jobs 1 when --jobs is specified.\n",
              stderr);
        return EXIT_FAILURE;
    }
    if (options.random_order && !options.seed_given) {
        options.seed = default_random_seed();
    }
    desired_jobs = options.node_limit != 0 ? 1U :
        (options.jobs != 0 ? options.jobs :
         (uint64_t)options.threads * DEFAULT_JOBS_PER_THREAD);
    if (desired_jobs == 0 || desired_jobs > SIZE_MAX / FACE_COUNT / DICE) {
        fputs("Requested job count is too large.\n", stderr);
        return EXIT_FAILURE;
    }
    if (!install_signal_handler()) {
        return EXIT_FAILURE;
    }
    if (!initialize_states_and_edges(&prototype) ||
        !initialize_targets(&prototype)) {
        fputs("Unable to initialize residual search.\n", stderr);
        return EXIT_FAILURE;
    }
    if (!prototype.possible) {
        fputs("Search complete: 0.000s, nodes=0, negative-prunes=0, "
              "right-end-prunes=0, left-end-prunes=0, "
              "permutation-fair=0\n", stderr);
        return EXIT_SUCCESS;
    }
    for (owner = 0; owner < DICE; ++owner) {
        prototype.remaining[owner] = SIDES;
    }
    prototype.encoding[FACE_COUNT] = '\0';
    start_time = monotonic_seconds();
    if (!build_prefixes(&prototype, &shared.prefixes, desired_jobs)) {
        if (interrupt_requested) {
            fprintf(stderr,
                    "Search interrupted during prefix-job setup: %.3fs, "
                    "configuration=%dd%d, order=%s",
                    monotonic_seconds() - start_time, DICE, SIDES,
                    options.random_order ? "random" : "canonical");
            if (options.random_order) {
                fprintf(stderr, ", seed=%" PRIu64, options.seed);
            }
            fputc('\n', stderr);
        } else {
            fputs("Unable to build residual-search prefix jobs.\n", stderr);
        }
        free(shared.prefixes.owners);
        return interrupt_requested ? 128 + SIGINT : EXIT_FAILURE;
    }
    shared.options = options;
    shared.setup_nodes = prototype.nodes;
    shared.setup_negative_prunes = prototype.negative_prunes;
    shared.setup_right_end_prunes = prototype.right_end_prunes;
    shared.setup_left_end_prunes = prototype.left_end_prunes;
    shared.job_count = shared.prefixes.count < desired_jobs
        ? shared.prefixes.count : desired_jobs;
    if (shared.job_count != desired_jobs) {
        fprintf(stderr,
                "Requested %" PRIu64 " jobs, but only %" PRIu64
                " viable prefix jobs exist; using that maximum.\n",
                desired_jobs, shared.job_count);
    }
    if (shared.job_count != 0) {
        if (shared.job_count > SIZE_MAX / sizeof(*shared.job_order)) {
            fputs("Job-order table is too large.\n", stderr);
            free(shared.prefixes.owners);
            return EXIT_FAILURE;
        }
        shared.job_order = malloc((size_t)shared.job_count *
                                  sizeof(*shared.job_order));
        if (shared.job_order == NULL) {
            fputs("Unable to allocate job-order table.\n", stderr);
            free(shared.prefixes.owners);
            return EXIT_FAILURE;
        }
        for (uint64_t job = 0; job < shared.job_count; ++job) {
            shared.job_order[job] = job;
        }
        if (options.random_order) {
            shuffle_jobs(&shared);
        }
    }
    atomic_init(&shared.next_job, 0);
    atomic_init(&shared.jobs_done, 0);
    atomic_init(&shared.configurations, 0);
    atomic_init(&shared.active_workers, 0);
    atomic_init(&shared.stop, false);
    atomic_init(&shared.internal_error, false);
    atomic_init(&shared.node_limit_reached, false);
    if (pthread_mutex_init(&shared.solution_mutex, NULL) != 0) {
        fputs("Unable to initialize solution mutex.\n", stderr);
        free(shared.job_order);
        free(shared.prefixes.owners);
        return EXIT_FAILURE;
    }
    if (pthread_mutex_init(&shared.event_mutex, NULL) != 0) {
        fputs("Unable to initialize worker-event mutex.\n", stderr);
        pthread_mutex_destroy(&shared.solution_mutex);
        free(shared.job_order);
        free(shared.prefixes.owners);
        return EXIT_FAILURE;
    }
    if (pthread_cond_init(&shared.event_condition, NULL) != 0) {
        fputs("Unable to initialize worker-event condition.\n", stderr);
        pthread_mutex_destroy(&shared.event_mutex);
        pthread_mutex_destroy(&shared.solution_mutex);
        free(shared.job_order);
        free(shared.prefixes.owners);
        return EXIT_FAILURE;
    }
    worker_count = options.threads;
    if ((uint64_t)worker_count > shared.job_count) {
        worker_count = (unsigned)shared.job_count;
    }
    if (worker_count != 0) {
        workers = calloc(worker_count, sizeof(*workers));
        if (workers == NULL) {
            fputs("Unable to allocate worker state.\n", stderr);
            exit_status = EXIT_FAILURE;
            goto cleanup;
        }
    }
    for (worker = 0; worker < worker_count; ++worker) {
        workers[worker].shared = &shared;
        workers[worker].search = prototype;
        workers[worker].search.nodes = 0;
        workers[worker].search.negative_prunes = 0;
        workers[worker].search.right_end_prunes = 0;
        workers[worker].search.left_end_prunes = 0;
        workers[worker].search.configurations = 0;
        workers[worker].search.internal_error = false;
        workers[worker].search.node_base = shared.setup_nodes;
        workers[worker].search.shared = &shared;
        workers[worker].search.published = &workers[worker].stats;
        atomic_init(&workers[worker].stats.nodes, 0);
        atomic_init(&workers[worker].stats.negative_prunes, 0);
        atomic_init(&workers[worker].stats.right_end_prunes, 0);
        atomic_init(&workers[worker].stats.left_end_prunes, 0);
    }

    fprintf(stderr,
            "Searching essentially different %dd%d configurations by "
            "residual-prefix search (%u states; right-end checks %s; "
            "two-end fail-first %s) with "
            "%u pthread workers (%" PRIu64 " jobs over %" PRIu64
            " prefixes, split depth %u; order=%s",
            DICE, SIDES, PERM_STATE_COUNT,
            RESIDUAL_RIGHT_END_CHECK ? "enabled" : "disabled",
            RESIDUAL_TWO_END_SEARCH ? "enabled" : "disabled",
            worker_count, shared.job_count, shared.prefixes.count,
            shared.prefixes.depth,
            options.random_order ? "random" : "canonical");
    if (options.random_order) {
        fprintf(stderr, ", seed=%" PRIu64, options.seed);
    }
    fputs(")\n", stderr);

    for (worker = 0; worker < worker_count; ++worker) {
        int result;

        atomic_fetch_add_explicit(&shared.active_workers, 1,
                                  memory_order_relaxed);
        result = pthread_create(&workers[worker].thread, NULL, worker_main,
                                &workers[worker]);
        if (result != 0) {
            fprintf(stderr, "Unable to create worker %u: %s\n", worker,
                    strerror(result));
            atomic_fetch_sub_explicit(&shared.active_workers, 1,
                                      memory_order_relaxed);
            atomic_store_explicit(&shared.stop, true, memory_order_relaxed);
            exit_status = EXIT_FAILURE;
            break;
        }
        ++created_workers;
    }
    if (created_workers != 0) {
        watch_workers(&shared, workers, worker_count, start_time);
    }
    for (worker = 0; worker < created_workers; ++worker) {
        int result = pthread_join(workers[worker].thread, NULL);

        if (result != 0) {
            fprintf(stderr, "Unable to join worker %u: %s\n", worker,
                    strerror(result));
            exit_status = EXIT_FAILURE;
        }
    }
    drain_solutions(&shared);
    if (atomic_load_explicit(&shared.internal_error, memory_order_relaxed)) {
        fputs("Residual search failed an internal verification check.\n",
              stderr);
        exit_status = EXIT_FAILURE;
    }
    {
        struct totals totals = collect_totals(&shared, workers, worker_count);
        uint64_t jobs_done = atomic_load_explicit(&shared.jobs_done,
                                                  memory_order_relaxed);
        bool node_limit_reached = atomic_load_explicit(
            &shared.node_limit_reached, memory_order_relaxed);
        bool solution_limit_reached = options.limit != 0 &&
            totals.configurations >= options.limit;
        const char *status;

        if (!interrupt_requested && !node_limit_reached &&
            !solution_limit_reached && exit_status == EXIT_SUCCESS &&
            jobs_done != shared.job_count) {
            fputs("Search ended before every job completed.\n", stderr);
            exit_status = EXIT_FAILURE;
        }
        status = interrupt_requested ? "Search interrupted" :
            node_limit_reached ? "Search stopped at node limit" :
            solution_limit_reached ? "Search stopped at solution limit" :
            exit_status != EXIT_SUCCESS ? "Search failed" :
                                          "Search complete";

        fprintf(stderr,
                "%s: %.3fs, configuration=%dd%d, workers=%u, jobs=%" PRIu64
                "/%" PRIu64 ", split-depth=%u, order=%s",
                status, monotonic_seconds() - start_time, DICE, SIDES,
                worker_count, jobs_done, shared.job_count,
                shared.prefixes.depth,
                options.random_order ? "random" : "canonical");
        if (options.random_order) {
            fprintf(stderr, ", seed=%" PRIu64, options.seed);
        }
        fprintf(stderr,
                ", nodes=%" PRIu64 ", negative-prunes=%" PRIu64
                ", right-end-prunes=%" PRIu64
                ", left-end-prunes=%" PRIu64
                ", permutation-fair=%" PRIu64 "\n",
                totals.nodes, totals.negative_prunes,
                totals.right_end_prunes, totals.left_end_prunes,
                totals.configurations);
    }
    if (interrupt_requested && exit_status == EXIT_SUCCESS) {
        exit_status = 128 + SIGINT;
    }

cleanup:
    drain_solutions(&shared);
    free(workers);
    free(shared.job_order);
    free(shared.prefixes.owners);
    pthread_cond_destroy(&shared.event_condition);
    pthread_mutex_destroy(&shared.event_mutex);
    pthread_mutex_destroy(&shared.solution_mutex);
    return exit_status;
}
