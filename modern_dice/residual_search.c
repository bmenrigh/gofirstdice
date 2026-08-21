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
 *
 * --extend-solutions uses a separate recursive path.  It preserves each
 * input (DICE-1)-die encoding as a canonical subsequence, pads those dice to
 * SIDES, and inserts a new die in every possible canonical first-occurrence
 * position.  Its cursor and planner do not add branches or state to the
 * exhaustive recursion above.
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

#ifndef MIRROR
#define MIRROR 0
#endif

#ifndef COLUMN_GROUP
#define COLUMN_GROUP 0
#endif

#ifndef RESIDUAL_RIGHT_END_CHECK
#define RESIDUAL_RIGHT_END_CHECK 1
#endif

#ifndef RESIDUAL_TWO_END_SEARCH
#define RESIDUAL_TWO_END_SEARCH 1
#endif

#ifndef RESIDUAL_EXTENSION
#define RESIDUAL_EXTENSION 1
#endif

#if MIRROR != 0 && MIRROR != 1
#error "MIRROR must be either 0 or 1"
#endif

#if COLUMN_GROUP != 0 && COLUMN_GROUP != 1
#error "COLUMN_GROUP must be either 0 or 1"
#endif

#if RESIDUAL_EXTENSION != 0 && RESIDUAL_EXTENSION != 1
#error "RESIDUAL_EXTENSION must be either 0 or 1"
#endif

#if MIRROR && ((SIDES % 2) != 0)
#error "Mirror-symmetric dice require an even SIDES value"
#endif

#define FACE_COUNT (DICE * SIDES)
#define BASE_DICE (DICE - 1U)
#define DEFAULT_PRINT_LIMIT UINT64_C(10)
#define DEFAULT_JOBS_PER_THREAD UINT64_C(8)
#define DEFAULT_PROGRESS_SECONDS UINT64_C(1)
#define PUBLISH_NODE_INTERVAL UINT64_C(1048576)
#define STOP_POLL_INTERVAL UINT64_C(4096)
#define PREFIX_RIGHT_FLAG UINT8_C(0x80)
#if MIRROR
#define SEARCH_STEP_FACES 2U
#else
#define SEARCH_STEP_FACES 1U
#endif
#define SEARCH_DECISION_COUNT (FACE_COUNT / SEARCH_STEP_FACES)
#define ALL_OWNER_MASK ((1U << DICE) - 1U)

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
    uint64_t progress_seconds;
    const char *extend_solutions_path;
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
    uint64_t *base_indices;
    uint8_t *new_owners;
    uint64_t count;
    uint64_t capacity;
    unsigned depth;
};

#if RESIDUAL_EXTENSION
struct extension_base {
    uint8_t *owners;
    uint16_t sides;
    uint16_t length;
};

struct extension_base_list {
    struct extension_base *items;
    uint64_t count;
    uint64_t capacity;
    uint64_t duplicate_count;
    uint64_t lesser_count;
    uint64_t same_side_nonfair_count;
    unsigned minimum_sides;
    unsigned maximum_sides;
};

struct extension_cursor {
    const struct extension_base *base;
    uint16_t padding_remaining[DICE];
    uint8_t base_owner_map[BASE_DICE];
    unsigned base_left;
    unsigned base_right;
    unsigned left_depth;
    unsigned right_depth;
    unsigned used_labels;
};
#endif

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

#if RESIDUAL_EXTENSION
struct solution_set {
    char **slots;
    size_t count;
    size_t capacity;
};
#endif

struct shared_state {
    struct options options;
    struct prefix_list prefixes;
#if RESIDUAL_EXTENSION
    struct extension_base_list extension_bases;
#endif
    uint64_t *job_order;
    uint64_t job_count;
    uint64_t setup_nodes;
    uint64_t setup_negative_prunes;
    uint64_t setup_right_end_prunes;
    uint64_t setup_left_end_prunes;
    atomic_uint_fast64_t next_job;
    atomic_uint_fast64_t jobs_done;
    atomic_uint_fast64_t configurations;
    atomic_uint_fast64_t duplicate_configurations;
    atomic_uint active_workers;
    atomic_bool stop;
    atomic_bool internal_error;
    atomic_bool node_limit_reached;
    pthread_mutex_t solution_mutex;
    pthread_mutex_t event_mutex;
    pthread_cond_t event_condition;
    struct solution *solution_head;
    struct solution *solution_tail;
#if RESIDUAL_EXTENSION
    struct solution_set extension_solutions;
#endif
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
    uint64_t duplicate_configurations;
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
            "Exhaustively search canonical %s%s%dd%d owner strings using "
            "exact "
            "residual roll counts.\n\n"
            "  -t, --threads N     worker threads; default is online CPUs\n"
            "  -j, --jobs N        logical prefix jobs; default is 8 per worker\n"
            "  -n, --limit N       stop after N configurations\n"
            "      --node-limit N  stop after visiting N search nodes\n"
            "      --print-limit N print at most N configurations\n"
            "      --all-solutions print every configuration\n"
            "      --random-order  shuffle logical jobs before searching\n"
            "      --seed N        random-order seed; requires --random-order\n"
            "      --extend-solutions FILE\n"
            "                      extend canonical (DICE-1)-die encodings\n"
            "                      from FILE by padding to SIDES and adding\n"
            "                      one new die in every first-occurrence\n"
            "                      position\n"
            "  -p, --progress N    progress interval in seconds; 0 disables\n"
            "  -q, --quiet         suppress configuration output\n"
            "  -h, --help          show this help\n",
            program, MIRROR ? "mirrored " : "",
            COLUMN_GROUP ? "column-grouped " : "", DICE, SIDES);
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
        .progress_seconds = DEFAULT_PROGRESS_SECONDS,
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
        } else if (strcmp(option, "-p") == 0 ||
                   strcmp(option, "--progress") == 0) {
            if (++argument == argc ||
                !parse_uint64(argv[argument], &options->progress_seconds)) {
                fputs("Invalid progress interval.\n", stderr);
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
        } else if (strcmp(option, "--extend-solutions") == 0) {
            if (++argument == argc || *argv[argument] == '\0' ||
                options->extend_solutions_path != NULL) {
                fputs("--extend-solutions requires one non-empty file path.\n",
                      stderr);
                return false;
            }
            options->extend_solutions_path = argv[argument];
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

#if RESIDUAL_EXTENSION
static void free_extension_bases(struct extension_base_list *bases)
{
    uint64_t index;

    for (index = 0; index < bases->count; ++index) {
        free(bases->items[index].owners);
    }
    free(bases->items);
    *bases = (struct extension_base_list){0};
}

static bool canonicalize_extension_base(const char *text, size_t length,
                                        struct extension_base *base)
{
    uint8_t owner_map[UCHAR_MAX + 1U];
    uint16_t owner_count[DICE] = {0};
    uint8_t *owners;
    unsigned next_owner = 0;
    unsigned sides;
    size_t face;

    if (length == 0 || length > BASE_DICE * (size_t)SIDES ||
        length % BASE_DICE != 0) {
        return false;
    }
    sides = (unsigned)(length / BASE_DICE);
    if (sides == 0 || sides > SIDES) {
        return false;
    }
    memset(owner_map, UINT8_MAX, sizeof(owner_map));
    owners = malloc(length);
    if (owners == NULL) {
        return false;
    }
    for (face = 0; face < length; ++face) {
        unsigned char symbol = (unsigned char)text[face];
        unsigned owner;

        if (!((symbol >= (unsigned char)'a' &&
               symbol <= (unsigned char)'z') ||
              (symbol >= (unsigned char)'A' &&
               symbol <= (unsigned char)'Z'))) {
            free(owners);
            return false;
        }
        if (owner_map[symbol] == UINT8_MAX) {
            if (next_owner >= BASE_DICE) {
                free(owners);
                return false;
            }
            owner_map[symbol] = (uint8_t)next_owner++;
        }
        owner = owner_map[symbol];
        owners[face] = (uint8_t)owner;
        ++owner_count[owner];
    }
    if (next_owner != BASE_DICE) {
        free(owners);
        return false;
    }
    for (unsigned owner = 0; owner < BASE_DICE; ++owner) {
        if (owner_count[owner] != sides) {
            free(owners);
            return false;
        }
    }
    base->owners = owners;
    base->sides = (uint16_t)sides;
    base->length = (uint16_t)length;
    return true;
}

static bool extension_base_is_permutation_fair(
    const struct search *search, const struct extension_base *base)
{
    uint64_t ways[PERM_STATE_COUNT] = {0};
    uint64_t base_target[DICE] = {0};
    uint64_t factorial = 1;
    unsigned length;
    unsigned face;
    size_t state;

    base_target[0] = 1;
    for (length = 1; length <= BASE_DICE; ++length) {
        uint64_t outcomes;

        factorial *= length;
        if (!integer_power(base->sides, length, &outcomes) ||
            outcomes % factorial != 0) {
            return false;
        }
        base_target[length] = outcomes / factorial;
    }
    ways[0] = 1;
    for (face = 0; face < base->length; ++face) {
        unsigned owner = (unsigned)base->owners[face] + 1U;
        unsigned edge;

        for (edge = 0; edge < OWNER_EDGE_COUNT; ++edge) {
            const struct edge *transition = &search->append[owner][edge];

            ways[transition->destination] += ways[transition->source];
        }
    }
    for (state = 0; state < PERM_STATE_COUNT; ++state) {
        if ((search->state[state].mask & 1U) == 0 &&
            ways[state] != base_target[search->state[state].length]) {
            return false;
        }
    }
    return true;
}

static bool append_extension_base(struct extension_base_list *bases,
                                  struct extension_base base)
{
    if (bases->count == bases->capacity) {
        uint64_t capacity = bases->capacity == 0 ? UINT64_C(64) :
            bases->capacity * 2U;
        struct extension_base *items;

        if (capacity < bases->capacity ||
            capacity > SIZE_MAX / sizeof(*bases->items)) {
            return false;
        }
        items = realloc(bases->items,
                        (size_t)capacity * sizeof(*bases->items));
        if (items == NULL) {
            return false;
        }
        bases->items = items;
        bases->capacity = capacity;
    }
    bases->items[bases->count++] = base;
    return true;
}

static int compare_extension_bases(const void *left_pointer,
                                   const void *right_pointer)
{
    const struct extension_base *left = left_pointer;
    const struct extension_base *right = right_pointer;
    int comparison;

    if (left->sides != right->sides) {
        return left->sides < right->sides ? -1 : 1;
    }
    comparison = memcmp(left->owners, right->owners, left->length);
    return comparison < 0 ? -1 : comparison > 0 ? 1 : 0;
}

static void deduplicate_extension_bases(struct extension_base_list *bases)
{
    uint64_t read;
    uint64_t write = 0;

    qsort(bases->items, (size_t)bases->count, sizeof(*bases->items),
          compare_extension_bases);
    for (read = 0; read < bases->count; ++read) {
        if (write != 0 &&
            compare_extension_bases(&bases->items[write - 1U],
                                    &bases->items[read]) == 0) {
            free(bases->items[read].owners);
            ++bases->duplicate_count;
            continue;
        }
        if (write != read) {
            bases->items[write] = bases->items[read];
        }
        ++write;
    }
    bases->count = write;
    if (write != 0) {
        bases->minimum_sides = bases->items[0].sides;
        bases->maximum_sides = bases->items[write - 1U].sides;
    }
}

static bool load_extension_bases(const char *path, const struct search *search,
                                 struct extension_base_list *bases)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    uint64_t line_number = 0;
    bool valid = true;

    if (file == NULL) {
        fprintf(stderr, "Unable to open extension solutions file %s: %s\n",
                path, strerror(errno));
        return false;
    }
    while (getline(&line, &capacity, file) >= 0) {
        const char *marker;
        const char *encoding;
        size_t length;
        struct extension_base base = {0};

        if (interrupt_requested) {
            valid = false;
            break;
        }
        ++line_number;
        marker = strstr(line, "encoding=");
        if (marker == NULL) {
            continue;
        }
        encoding = marker + strlen("encoding=");
        length = strcspn(encoding, " \t\r\n");
        if (!canonicalize_extension_base(encoding, length, &base)) {
            fprintf(stderr,
                    "%s:%" PRIu64 ": encoding is not a balanced "
                    "%u-die configuration with at most %d sides.\n",
                    path, line_number, BASE_DICE, SIDES);
            valid = false;
            break;
        }
        if (!extension_base_is_permutation_fair(search, &base)) {
            if (base.sides == SIDES) {
                free(base.owners);
                ++bases->same_side_nonfair_count;
                continue;
            }
            ++bases->lesser_count;
        }
        if (!append_extension_base(bases, base)) {
            free(base.owners);
            fputs("Unable to allocate extension-base table.\n", stderr);
            valid = false;
            break;
        }
    }
    if (ferror(file)) {
        fprintf(stderr, "Unable to read extension solutions file %s: %s\n",
                path, strerror(errno));
        valid = false;
    }
    free(line);
    if (fclose(file) != 0 && valid) {
        fprintf(stderr, "Unable to close extension solutions file %s: %s\n",
                path, strerror(errno));
        valid = false;
    }
    if (!valid) {
        free_extension_bases(bases);
        return false;
    }
    if (bases->count == 0) {
        fprintf(stderr,
                "Extension solutions file %s contains no usable "
                "balanced %u-die encodings.\n",
                path, BASE_DICE);
        free_extension_bases(bases);
        return false;
    }
    deduplicate_extension_bases(bases);
    return true;
}
#endif

static void free_shared_extension_bases(struct shared_state *shared)
{
#if RESIDUAL_EXTENSION
    free_extension_bases(&shared->extension_bases);
#else
    (void)shared;
#endif
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

#if RESIDUAL_TWO_END_SEARCH || MIRROR
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

#if !MIRROR
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
#endif

#if !MIRROR
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
#endif

static unsigned owner_mask_count(unsigned mask)
{
    unsigned count = 0;

    while (mask != 0) {
        mask &= mask - 1U;
        ++count;
    }
    return count;
}

static bool column_fronts_share_group(unsigned left_depth,
                                      unsigned right_depth)
{
#if COLUMN_GROUP
    return left_depth + right_depth < FACE_COUNT &&
        left_depth / DICE ==
            (FACE_COUNT - 1U - right_depth) / DICE;
#else
    (void)left_depth;
    (void)right_depth;
    return false;
#endif
}

static unsigned column_allowed_owner_mask(unsigned used_owner_mask,
                                          unsigned opposite_owner_mask,
                                          bool shared_group)
{
#if COLUMN_GROUP
    unsigned blocked = used_owner_mask |
        (shared_group ? opposite_owner_mask : 0U);

    return ALL_OWNER_MASK & ~blocked;
#else
    (void)used_owner_mask;
    (void)opposite_owner_mask;
    (void)shared_group;
    return ALL_OWNER_MASK;
#endif
}

static unsigned column_mask_after_owner(unsigned used_owner_mask,
                                        unsigned owner)
{
#if COLUMN_GROUP
    unsigned next = used_owner_mask | (1U << owner);

    return next == ALL_OWNER_MASK ? 0U : next;
#else
    (void)used_owner_mask;
    (void)owner;
    return 0U;
#endif
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
#if !MIRROR
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
                                       unsigned used_labels,
                                       unsigned left_depth,
                                       unsigned right_depth,
                                       unsigned left_column_mask,
                                       unsigned right_column_mask)
{
    unsigned owner_limit = used_labels < DICE ? used_labels + 1U : DICE;
    unsigned available = available_owner_mask(search, owner_limit);
    bool shared_group = column_fronts_share_group(left_depth, right_depth);
    unsigned left_available = available &
        column_allowed_owner_mask(left_column_mask, right_column_mask,
                                  shared_group);
    unsigned left_mask = feasible_owner_mask(search, search->prepend,
                                             owner_limit) & left_available;
    struct end_plan plan = {
        .end = SEARCH_LEFT,
        .owner_mask = left_mask,
    };

    if (used_labels < DICE) {
        plan.rejected_owners = owner_mask_count(left_available) -
            owner_mask_count(left_mask);
        return plan;
    }
    {
        unsigned right_available = available &
            column_allowed_owner_mask(right_column_mask, left_column_mask,
                                      shared_group);
        unsigned right_mask = feasible_owner_mask(search, search->append,
                                                  DICE) & right_available;

        if (owner_mask_count(right_mask) < owner_mask_count(left_mask)) {
            plan.end = SEARCH_RIGHT;
            plan.owner_mask = right_mask;
            plan.rejected_owners = owner_mask_count(right_available) -
                owner_mask_count(right_mask);
            return plan;
        }
    }
    plan.rejected_owners = owner_mask_count(left_available) -
        owner_mask_count(left_mask);
    return plan;
}
#else
static struct end_plan plan_search_end(const struct search *search,
                                       unsigned used_labels,
                                       unsigned left_depth,
                                       unsigned right_depth,
                                       unsigned left_column_mask,
                                       unsigned right_column_mask)
{
    unsigned owner_limit = used_labels < DICE ? used_labels + 1U : DICE;
    bool shared_group = column_fronts_share_group(left_depth, right_depth);
    unsigned available = available_owner_mask(search, owner_limit) &
        column_allowed_owner_mask(left_column_mask, right_column_mask,
                                  shared_group);
    unsigned feasible = feasible_owner_mask(search, search->prepend,
                                            owner_limit) & available;

    return (struct end_plan){
        .end = SEARCH_LEFT,
        .owner_mask = feasible,
        .rejected_owners = owner_mask_count(available) -
            owner_mask_count(feasible),
    };
}
#endif
#endif

#if RESIDUAL_EXTENSION && !MIRROR && !COLUMN_GROUP
static unsigned extension_mapped_base_owner(
    const struct extension_cursor *cursor, unsigned base_owner)
{
    return cursor->base_owner_map[base_owner];
}

static unsigned extension_fixed_owner(const struct extension_cursor *cursor,
                                      enum search_end end)
{
    unsigned base_position;

    if (cursor->base_left == cursor->base_right) {
        return DICE;
    }
    base_position = end == SEARCH_LEFT ? cursor->base_left :
        cursor->base_right - 1U;
    return extension_mapped_base_owner(
        cursor, cursor->base->owners[base_position]);
}

static unsigned extension_legal_owner_mask(
    const struct extension_cursor *cursor, enum search_end end)
{
    unsigned owner_limit = end == SEARCH_LEFT && cursor->used_labels < DICE
        ? cursor->used_labels + 1U : DICE;
    unsigned fixed_owner = extension_fixed_owner(cursor, end);
    unsigned mask = 0;
    unsigned owner;

    for (owner = 0; owner < owner_limit; ++owner) {
        if (cursor->padding_remaining[owner] != 0) {
            mask |= 1U << owner;
        }
    }
    if (fixed_owner < owner_limit) {
        /* Matching an exposed skeleton face always consumes it.  Excluding
         * the indistinguishable padding move gives every interleaving one
         * canonical embedding of the fixed skeleton. */
        mask |= 1U << fixed_owner;
    }
    return mask;
}

static unsigned feasible_extension_owner_mask(
    const struct search *search,
    const struct edge (*edges)[OWNER_EDGE_COUNT], unsigned candidates)
{
    unsigned feasible = 0;

    while (candidates != 0) {
        unsigned owner = (unsigned)__builtin_ctz(candidates);
        unsigned edge;

        candidates &= candidates - 1U;
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
            feasible |= 1U << owner;
        }
    }
    return feasible;
}

static struct end_plan plan_extension_end(
    const struct search *search, const struct extension_cursor *cursor)
{
    unsigned left_legal = extension_legal_owner_mask(cursor, SEARCH_LEFT);
    unsigned left_feasible = feasible_extension_owner_mask(
        search, search->prepend, left_legal);
    struct end_plan plan = {
        .end = SEARCH_LEFT,
        .owner_mask = left_feasible,
        .rejected_owners = owner_mask_count(left_legal) -
            owner_mask_count(left_feasible),
    };

    /* First occurrences, including the selected position of the new die,
     * are established from the left.  Once every label exists, either end
     * can be peeled without introducing label-symmetry duplicates. */
    if (cursor->used_labels < DICE) {
        return plan;
    }
    {
        unsigned right_legal = extension_legal_owner_mask(
            cursor, SEARCH_RIGHT);
        unsigned right_feasible = feasible_extension_owner_mask(
            search, search->append, right_legal);

        if (owner_mask_count(right_feasible) <
            owner_mask_count(left_feasible)) {
            plan.end = SEARCH_RIGHT;
            plan.owner_mask = right_feasible;
            plan.rejected_owners = owner_mask_count(right_legal) -
                owner_mask_count(right_feasible);
        }
    }
    return plan;
}

static bool apply_extension_owner(struct search *search,
                                  struct extension_cursor *cursor,
                                  enum search_end end, unsigned owner,
                                  bool *fixed, bool *introduced)
{
    unsigned fixed_owner = extension_fixed_owner(cursor, end);
    unsigned position = end == SEARCH_LEFT ? cursor->left_depth :
        FACE_COUNT - 1U - cursor->right_depth;

    *fixed = owner == fixed_owner;
    *introduced = end == SEARCH_LEFT && owner == cursor->used_labels;
    if (*fixed) {
        if (end == SEARCH_LEFT) {
            ++cursor->base_left;
        } else {
            --cursor->base_right;
        }
    } else {
        if (cursor->padding_remaining[owner] == 0) {
            return false;
        }
        --cursor->padding_remaining[owner];
    }
    --search->remaining[owner];
    search->encoding[position] = (char)('a' + owner);
    if (end == SEARCH_LEFT) {
        subtract_prefix_owner(search, owner);
        ++cursor->left_depth;
        if (*introduced) {
            ++cursor->used_labels;
        }
    } else {
        subtract_suffix_owner(search, owner);
        ++cursor->right_depth;
    }
    return true;
}

static void undo_extension_owner(struct search *search,
                                 struct extension_cursor *cursor,
                                 enum search_end end, unsigned owner,
                                 bool fixed, bool introduced)
{
    if (end == SEARCH_LEFT) {
        restore_prefix_owner(search, owner);
        --cursor->left_depth;
        if (introduced) {
            --cursor->used_labels;
        }
    } else {
        restore_suffix_owner(search, owner);
        --cursor->right_depth;
    }
    ++search->remaining[owner];
    if (fixed) {
        if (end == SEARCH_LEFT) {
            --cursor->base_left;
        } else {
            ++cursor->base_right;
        }
    } else {
        ++cursor->padding_remaining[owner];
    }
}

static void initialize_extension_cursor(const struct extension_base *base,
                                        unsigned new_owner,
                                        struct extension_cursor *cursor)
{
    unsigned base_owner;
    unsigned owner;

    *cursor = (struct extension_cursor){
        .base = base,
        .base_right = base->length,
    };
    /* Embed the canonical base labels around the selected missing label.
     * The ordinary used-label rule then forces the new die's first face into
     * that exact first-occurrence position. */
    for (base_owner = 0; base_owner < BASE_DICE; ++base_owner) {
        cursor->base_owner_map[base_owner] = (uint8_t)(
            base_owner + (base_owner >= new_owner ? 1U : 0U));
    }
    for (owner = 0; owner < DICE; ++owner) {
        cursor->padding_remaining[owner] = owner == new_owner
            ? SIDES : (uint16_t)(SIDES - base->sides);
    }
}
#endif

#if MIRROR
/* A mirrored decision removes the same owner from both residual ends. */
static struct end_plan plan_mirror_pairs(struct search *search,
                                         unsigned used_labels,
                                         unsigned left_depth,
                                         unsigned right_depth,
                                         unsigned left_column_mask,
                                         unsigned right_column_mask)
{
    unsigned owner_limit = used_labels < DICE ? used_labels + 1U : DICE;
    bool shared_group = column_fronts_share_group(left_depth, right_depth);
    unsigned allowed = column_allowed_owner_mask(
        left_column_mask, right_column_mask, shared_group) &
        column_allowed_owner_mask(
            right_column_mask, left_column_mask, shared_group);
    unsigned available = 0;
    unsigned feasible = 0;
    unsigned owner;

    /*
     * Prepend and append updates for one owner have disjoint destinations
     * except for the singleton counter.  remaining >= 2 proves that shared
     * counter can absorb both updates, so no speculative update/restore is
     * needed while constructing the candidate mask.
     */
    for (owner = 0; owner < owner_limit; ++owner) {
        unsigned edge;

        if (search->remaining[owner] < 2U ||
            (allowed & (1U << owner)) == 0) {
            continue;
        }
        available |= 1U << owner;
        for (edge = 0; edge < OWNER_EDGE_COUNT; ++edge) {
            const struct edge *transition = &search->prepend[owner][edge];

            if (search->residual[transition->destination] <
                search->residual[transition->source]) {
                break;
            }
        }
        if (edge != OWNER_EDGE_COUNT) {
            continue;
        }
        /* At every mirror-pair boundary residual[q] equals
         * residual[reverse(q)].  Prepend and append feasibility are
         * therefore equivalent, so a second append-edge scan would only
         * repeat the work above. */
        feasible |= 1U << owner;
    }
    return (struct end_plan){
        .end = SEARCH_LEFT,
        .owner_mask = feasible,
        .rejected_owners = owner_mask_count(available) -
            owner_mask_count(feasible),
    };
}

static enum owner_result try_mirror_pair(struct search *search,
                                         unsigned owner,
                                         unsigned left_depth,
                                         unsigned right_depth)
{
    search->remaining[owner] -= 2U;
    search->encoding[left_depth] = (char)('a' + owner);
    search->encoding[FACE_COUNT - 1U - right_depth] =
        (char)('a' + owner);
    subtract_prefix_owner(search, owner);
    subtract_suffix_owner(search, owner);
    return OWNER_ACCEPTED;
}

static void undo_mirror_pair(struct search *search, unsigned owner)
{
    restore_suffix_owner(search, owner);
    restore_prefix_owner(search, owner);
    search->remaining[owner] += 2U;
}
#endif

static bool verify_configuration(const struct search *search)
{
    uint64_t ways[PERM_STATE_COUNT] = {0};
    unsigned face;
    size_t state;

#if COLUMN_GROUP
    for (face = 0; face < FACE_COUNT; face += DICE) {
        unsigned owner_mask = 0;
        unsigned offset;

        for (offset = 0; offset < DICE; ++offset) {
            unsigned owner =
                (unsigned)(search->encoding[face + offset] - 'a');

            if (owner >= DICE || (owner_mask & (1U << owner)) != 0) {
                return false;
            }
            owner_mask |= 1U << owner;
        }
        if (owner_mask != ALL_OWNER_MASK) {
            return false;
        }
    }
#endif
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

#if !MIRROR
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
#endif

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

#if RESIDUAL_EXTENSION
static uint64_t hash_encoding(const char *encoding)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned face;

    for (face = 0; face < FACE_COUNT; ++face) {
        hash ^= (unsigned char)encoding[face];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool grow_solution_set(struct solution_set *set)
{
    size_t capacity = set->capacity == 0 ? 128U : set->capacity * 2U;
    char **slots;
    size_t old_slot;

    if (capacity < set->capacity || capacity > SIZE_MAX / sizeof(*slots)) {
        return false;
    }
    slots = calloc(capacity, sizeof(*slots));
    if (slots == NULL) {
        return false;
    }
    for (old_slot = 0; old_slot < set->capacity; ++old_slot) {
        char *encoding = set->slots[old_slot];

        if (encoding != NULL) {
            size_t slot = (size_t)hash_encoding(encoding) & (capacity - 1U);

            while (slots[slot] != NULL) {
                slot = (slot + 1U) & (capacity - 1U);
            }
            slots[slot] = encoding;
        }
    }
    free(set->slots);
    set->slots = slots;
    set->capacity = capacity;
    return true;
}

/* Return 1 for a new encoding, 0 for a duplicate, and -1 on allocation
 * failure.  This is called only for fully verified extension solutions. */
static int remember_extension_solution(struct shared_state *shared,
                                       const char *encoding)
{
    struct solution_set *set = &shared->extension_solutions;
    size_t slot;
    char *copy;
    int result;

    pthread_mutex_lock(&shared->solution_mutex);
    if (set->capacity == 0 ||
        set->count + 1U > set->capacity - set->capacity / 4U) {
        if (!grow_solution_set(set)) {
            pthread_mutex_unlock(&shared->solution_mutex);
            return -1;
        }
    }
    slot = (size_t)hash_encoding(encoding) & (set->capacity - 1U);
    while (set->slots[slot] != NULL) {
        if (memcmp(set->slots[slot], encoding, FACE_COUNT) == 0) {
            pthread_mutex_unlock(&shared->solution_mutex);
            atomic_fetch_add_explicit(&shared->duplicate_configurations, 1,
                                      memory_order_relaxed);
            return 0;
        }
        slot = (slot + 1U) & (set->capacity - 1U);
    }
    copy = malloc(FACE_COUNT + 1U);
    if (copy == NULL) {
        result = -1;
    } else {
        memcpy(copy, encoding, FACE_COUNT + 1U);
        set->slots[slot] = copy;
        ++set->count;
        result = 1;
    }
    pthread_mutex_unlock(&shared->solution_mutex);
    return result;
}

static void free_solution_set(struct solution_set *set)
{
    size_t slot;

    for (slot = 0; slot < set->capacity; ++slot) {
        free(set->slots[slot]);
    }
    free(set->slots);
    *set = (struct solution_set){0};
}
#endif

static void free_shared_solution_set(struct shared_state *shared)
{
#if RESIDUAL_EXTENSION
    free_solution_set(&shared->extension_solutions);
#else
    (void)shared;
#endif
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
#if RESIDUAL_EXTENSION
    if (shared->options.extend_solutions_path != NULL) {
        int unique = remember_extension_solution(shared, search->encoding);

        if (unique == 0) {
            return true;
        }
        if (unique < 0) {
            search->internal_error = true;
            atomic_store_explicit(&shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&shared->stop, true, memory_order_relaxed);
            return false;
        }
    }
#endif
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
                                  unsigned used_labels,
                                  unsigned left_column_mask,
                                  unsigned right_column_mask)
{
    unsigned assigned = left_depth + right_depth;
    struct end_plan plan;
    enum search_end end;
#if !MIRROR
    unsigned position;
#endif
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

#if MIRROR
    plan = plan_mirror_pairs(search, used_labels, left_depth, right_depth,
                             left_column_mask, right_column_mask);
#else
    plan = plan_search_end(search, used_labels, left_depth, right_depth,
                           left_column_mask, right_column_mask);
#endif
    end = plan.end;
    search->negative_prunes += plan.rejected_owners;
#if !MIRROR
    position = end == SEARCH_LEFT ? left_depth :
        FACE_COUNT - 1U - right_depth;
#endif

    for (owner = 0; owner < DICE; ++owner) {
        bool new_label = owner == used_labels &&
            (MIRROR || end == SEARCH_LEFT);
        enum owner_result result;

        if ((plan.owner_mask & (1U << owner)) == 0) {
            continue;
        }
#if MIRROR
        result = try_mirror_pair(search, owner, left_depth, right_depth);
#else
        result = try_owner(search, owner, assigned, position, end);
#endif
        switch (result) {
        case OWNER_RIGHT_END_IMPOSSIBLE:
            ++search->right_end_prunes;
            break;
        case OWNER_LEFT_END_IMPOSSIBLE:
            ++search->left_end_prunes;
            break;
        case OWNER_ACCEPTED:
            search_configurations(
                search,
                left_depth + (MIRROR || end == SEARCH_LEFT ? 1U : 0U),
                right_depth + (MIRROR || end == SEARCH_RIGHT ? 1U : 0U),
                used_labels + (new_label ? 1U : 0U),
                MIRROR || end == SEARCH_LEFT ?
                    column_mask_after_owner(left_column_mask, owner) :
                    left_column_mask,
                MIRROR || end == SEARCH_RIGHT ?
                    column_mask_after_owner(right_column_mask, owner) :
                    right_column_mask);
#if MIRROR
            undo_mirror_pair(search, owner);
#else
            undo_owner(search, owner, end);
#endif
            break;
        }
        if (search->internal_error) {
            return;
        }
    }
}

#if RESIDUAL_EXTENSION && !MIRROR && !COLUMN_GROUP
static bool extension_cursor_is_complete(
    const struct extension_cursor *cursor)
{
    unsigned owner;

    if (cursor->base_left != cursor->base_right) {
        return false;
    }
    for (owner = 0; owner < DICE; ++owner) {
        if (cursor->padding_remaining[owner] != 0) {
            return false;
        }
    }
    return true;
}

static void search_extensions(struct search *search,
                              struct extension_cursor *cursor)
{
    unsigned assigned = cursor->left_depth + cursor->right_depth;
    struct end_plan plan;
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
        if (!extension_cursor_is_complete(cursor)) {
            search->internal_error = true;
            atomic_store_explicit(&search->shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&search->shared->stop, true,
                                  memory_order_relaxed);
            return;
        }
        (void)record_configuration(search);
        return;
    }

    plan = plan_extension_end(search, cursor);
    search->negative_prunes += plan.rejected_owners;
    for (owner = 0; owner < DICE; ++owner) {
        bool fixed;
        bool introduced;

        if ((plan.owner_mask & (1U << owner)) == 0) {
            continue;
        }
        if (!apply_extension_owner(search, cursor, plan.end, owner,
                                   &fixed, &introduced)) {
            search->internal_error = true;
            atomic_store_explicit(&search->shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&search->shared->stop, true,
                                  memory_order_relaxed);
            return;
        }
        search_extensions(search, cursor);
        undo_extension_owner(search, cursor, plan.end, owner, fixed,
                             introduced);
        if (search->internal_error) {
            return;
        }
    }
}
#endif

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

static void clear_prefixes(struct prefix_list *prefixes)
{
    free(prefixes->owners);
    free(prefixes->base_indices);
    free(prefixes->new_owners);
    *prefixes = (struct prefix_list){0};
}

#if RESIDUAL_EXTENSION && !MIRROR && !COLUMN_GROUP
static bool append_extension_prefix(struct prefix_list *prefixes,
                                    const uint8_t *steps,
                                    uint64_t base_index,
                                    unsigned new_owner)
{
    if (new_owner >= DICE) {
        return false;
    }
    if (prefixes->count == prefixes->capacity) {
        uint64_t capacity = prefixes->capacity == 0
            ? UINT64_C(64) : prefixes->capacity * 2U;
        uint8_t *owners = NULL;
        uint8_t *new_owners;
        uint64_t *base_indices;

        if (capacity < prefixes->capacity ||
            capacity > SIZE_MAX / sizeof(*base_indices) ||
            (prefixes->depth != 0 &&
             capacity > SIZE_MAX / prefixes->depth)) {
            return false;
        }
        base_indices = malloc((size_t)capacity * sizeof(*base_indices));
        if (base_indices == NULL) {
            return false;
        }
        new_owners = malloc((size_t)capacity * sizeof(*new_owners));
        if (new_owners == NULL) {
            free(base_indices);
            return false;
        }
        if (prefixes->depth != 0) {
            owners = malloc((size_t)capacity * prefixes->depth);
            if (owners == NULL) {
                free(new_owners);
                free(base_indices);
                return false;
            }
            if (prefixes->count != 0) {
                memcpy(owners, prefixes->owners,
                       (size_t)prefixes->count * prefixes->depth);
            }
        }
        if (prefixes->count != 0) {
            memcpy(base_indices, prefixes->base_indices,
                   (size_t)prefixes->count * sizeof(*base_indices));
            memcpy(new_owners, prefixes->new_owners,
                   (size_t)prefixes->count * sizeof(*new_owners));
        }
        free(prefixes->owners);
        free(prefixes->base_indices);
        free(prefixes->new_owners);
        prefixes->owners = owners;
        prefixes->base_indices = base_indices;
        prefixes->new_owners = new_owners;
        prefixes->capacity = capacity;
    }
    if (prefixes->depth != 0) {
        memcpy(prefixes->owners +
                   (size_t)prefixes->count * prefixes->depth,
               steps, prefixes->depth);
    }
    prefixes->base_indices[prefixes->count] = base_index;
    prefixes->new_owners[prefixes->count] = (uint8_t)new_owner;
    ++prefixes->count;
    return true;
}
#endif

static bool collect_prefixes(struct search *search,
                             struct prefix_list *prefixes, uint8_t *steps,
                             unsigned left_depth, unsigned right_depth,
                             unsigned used_labels,
                             unsigned left_column_mask,
                             unsigned right_column_mask)
{
    unsigned assigned = left_depth + right_depth;
    unsigned decision_depth = assigned / SEARCH_STEP_FACES;
    struct end_plan plan;
    enum search_end end;
#if !MIRROR
    unsigned position;
#endif
    unsigned owner;

    if (interrupt_requested) {
        return false;
    }
    if (decision_depth == prefixes->depth) {
        return append_prefix(prefixes, steps);
    }
    ++search->nodes;
#if MIRROR
    plan = plan_mirror_pairs(search, used_labels, left_depth, right_depth,
                             left_column_mask, right_column_mask);
#else
    plan = plan_search_end(search, used_labels, left_depth, right_depth,
                           left_column_mask, right_column_mask);
#endif
    end = plan.end;
    search->negative_prunes += plan.rejected_owners;
#if !MIRROR
    position = end == SEARCH_LEFT ? left_depth :
        FACE_COUNT - 1U - right_depth;
#endif
    for (owner = 0; owner < DICE; ++owner) {
        bool new_label = owner == used_labels &&
            (MIRROR || end == SEARCH_LEFT);
        enum owner_result result;

        if ((plan.owner_mask & (1U << owner)) == 0) {
            continue;
        }
        steps[decision_depth] = (uint8_t)owner |
            (end == SEARCH_RIGHT ? PREFIX_RIGHT_FLAG : 0U);
#if MIRROR
        result = try_mirror_pair(search, owner, left_depth, right_depth);
#else
        result = try_owner(search, owner, assigned, position, end);
#endif
        switch (result) {
        case OWNER_RIGHT_END_IMPOSSIBLE:
            ++search->right_end_prunes;
            break;
        case OWNER_LEFT_END_IMPOSSIBLE:
            ++search->left_end_prunes;
            break;
        case OWNER_ACCEPTED:
            if (!collect_prefixes(
                    search, prefixes, steps,
                    left_depth +
                        (MIRROR || end == SEARCH_LEFT ? 1U : 0U),
                    right_depth +
                        (MIRROR || end == SEARCH_RIGHT ? 1U : 0U),
                    used_labels + (new_label ? 1U : 0U),
                    MIRROR || end == SEARCH_LEFT ?
                        column_mask_after_owner(left_column_mask, owner) :
                        left_column_mask,
                    MIRROR || end == SEARCH_RIGHT ?
                        column_mask_after_owner(right_column_mask, owner) :
                        right_column_mask)) {
#if MIRROR
                undo_mirror_pair(search, owner);
#else
                undo_owner(search, owner, end);
#endif
                return false;
            }
#if MIRROR
            undo_mirror_pair(search, owner);
#else
            undo_owner(search, owner, end);
#endif
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

    for (depth = 1; depth <= SEARCH_DECISION_COUNT; ++depth) {
        clear_prefixes(prefixes);
        *prefixes = (struct prefix_list){.depth = depth};
        prototype->nodes = 0;
        prototype->negative_prunes = 0;
        prototype->right_end_prunes = 0;
        prototype->left_end_prunes = 0;
        if (!collect_prefixes(prototype, prefixes, steps, 0, 0, 0, 0, 0)) {
            return false;
        }
        if (prefixes->count == 0 || prefixes->count >= desired_jobs ||
            depth == SEARCH_DECISION_COUNT) {
            return true;
        }
    }
    return false;
}

#if RESIDUAL_EXTENSION && !MIRROR && !COLUMN_GROUP
static bool collect_extension_prefixes(
    struct search *search, struct extension_cursor *cursor,
    struct prefix_list *prefixes, uint8_t *steps, unsigned decision_depth,
    uint64_t base_index, unsigned new_owner)
{
    struct end_plan plan;
    unsigned owner;

    if (interrupt_requested) {
        return false;
    }
    if (decision_depth == prefixes->depth) {
        return append_extension_prefix(
            prefixes, steps, base_index, new_owner);
    }
    if (cursor->left_depth + cursor->right_depth == FACE_COUNT) {
        return true;
    }
    ++search->nodes;
    plan = plan_extension_end(search, cursor);
    search->negative_prunes += plan.rejected_owners;
    for (owner = 0; owner < DICE; ++owner) {
        bool fixed;
        bool introduced;

        if ((plan.owner_mask & (1U << owner)) == 0) {
            continue;
        }
        steps[decision_depth] = (uint8_t)owner |
            (plan.end == SEARCH_RIGHT ? PREFIX_RIGHT_FLAG : 0U);
        if (!apply_extension_owner(search, cursor, plan.end, owner,
                                   &fixed, &introduced)) {
            return false;
        }
        if (!collect_extension_prefixes(
                search, cursor, prefixes, steps, decision_depth + 1U,
                base_index, new_owner)) {
            undo_extension_owner(search, cursor, plan.end, owner, fixed,
                                 introduced);
            return false;
        }
        undo_extension_owner(search, cursor, plan.end, owner, fixed,
                             introduced);
    }
    return true;
}

static void reset_residual_configuration(struct search *search)
{
    size_t state;
    unsigned owner;

    for (state = 0; state < PERM_STATE_COUNT; ++state) {
        search->residual[state] =
            search->target[search->state[state].length];
    }
    for (owner = 0; owner < DICE; ++owner) {
        search->remaining[owner] = SIDES;
    }
}

static bool build_extension_prefixes(
    struct search *prototype, const struct extension_base_list *bases,
    struct prefix_list *prefixes, uint64_t desired_jobs)
{
    uint8_t steps[FACE_COUNT];
    unsigned depth;

    for (depth = 0; depth < FACE_COUNT; ++depth) {
        uint64_t base_index;

        clear_prefixes(prefixes);
        prefixes->depth = depth;
        prototype->nodes = 0;
        prototype->negative_prunes = 0;
        prototype->right_end_prunes = 0;
        prototype->left_end_prunes = 0;
        for (base_index = 0; base_index < bases->count; ++base_index) {
            unsigned new_owner;

            for (new_owner = 0; new_owner < DICE; ++new_owner) {
                struct extension_cursor cursor;

                reset_residual_configuration(prototype);
                initialize_extension_cursor(
                    &bases->items[base_index], new_owner, &cursor);
                if (!collect_extension_prefixes(
                        prototype, &cursor, prefixes, steps, 0, base_index,
                        new_owner)) {
                    return false;
                }
            }
        }
        if (prefixes->count == 0 || prefixes->count >= desired_jobs ||
            depth + 1U == FACE_COUNT) {
            reset_residual_configuration(prototype);
            return true;
        }
    }
    return false;
}
#endif

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
    unsigned left_column_mask = 0;
    unsigned right_column_mask = 0;
    unsigned applied = 0;
    unsigned depth;

    for (depth = 0; depth < prefixes->depth; ++depth) {
        uint8_t step = prefix[depth];
#if MIRROR
        struct end_plan plan = plan_mirror_pairs(
            search, used_labels, left_depth, right_depth,
            left_column_mask, right_column_mask);
#else
        struct end_plan plan = plan_search_end(
            search, used_labels, left_depth, right_depth,
            left_column_mask, right_column_mask);
#endif
        enum search_end end = (step & PREFIX_RIGHT_FLAG) != 0
            ? SEARCH_RIGHT : SEARCH_LEFT;
        unsigned owner = step & ~PREFIX_RIGHT_FLAG;
#if !MIRROR
        unsigned assigned = left_depth + right_depth;
        unsigned position = end == SEARCH_LEFT ? left_depth :
            FACE_COUNT - 1U - right_depth;
#endif
        enum owner_result result;

        if (owner >= DICE ||
            search->remaining[owner] < SEARCH_STEP_FACES ||
            end != plan.end || (plan.owner_mask & (1U << owner)) == 0) {
            search->internal_error = true;
            atomic_store_explicit(&search->shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&search->shared->stop, true,
                                  memory_order_relaxed);
            break;
        }
#if MIRROR
        result = try_mirror_pair(search, owner, left_depth, right_depth);
#else
        result = try_owner(search, owner, assigned, position, end);
#endif
        if (result != OWNER_ACCEPTED) {
            search->internal_error = true;
            atomic_store_explicit(&search->shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&search->shared->stop, true,
                                  memory_order_relaxed);
            break;
        }
        ++applied;
        left_depth += MIRROR || end == SEARCH_LEFT;
        right_depth += MIRROR || end == SEARCH_RIGHT;
        if (MIRROR || end == SEARCH_LEFT) {
            left_column_mask = column_mask_after_owner(left_column_mask,
                                                       owner);
        }
        if (MIRROR || end == SEARCH_RIGHT) {
            right_column_mask = column_mask_after_owner(right_column_mask,
                                                        owner);
        }
        if ((MIRROR || end == SEARCH_LEFT) && owner == used_labels) {
            ++used_labels;
        }
    }
    if (!search->internal_error) {
        search_configurations(search, left_depth, right_depth, used_labels,
                              left_column_mask, right_column_mask);
    }
    while (applied > 0) {
        uint8_t step;
#if !MIRROR
        enum search_end end;
#endif

        --applied;
        step = prefix[applied];
#if MIRROR
        undo_mirror_pair(search, step & ~PREFIX_RIGHT_FLAG);
#else
        end = (step & PREFIX_RIGHT_FLAG) != 0
            ? SEARCH_RIGHT : SEARCH_LEFT;
        undo_owner(search, step & ~PREFIX_RIGHT_FLAG, end);
#endif
    }
    return !search_should_stop(search);
}

#if RESIDUAL_EXTENSION && !MIRROR && !COLUMN_GROUP
static bool run_extension_prefix(struct search *search,
                                 uint64_t prefix_index)
{
    const struct prefix_list *prefixes = &search->shared->prefixes;
    const uint8_t *prefix = prefixes->depth == 0 ? NULL :
        prefixes->owners + (size_t)prefix_index * prefixes->depth;
    uint64_t base_index = prefixes->base_indices[prefix_index];
    unsigned new_owner = prefixes->new_owners[prefix_index];
    const struct extension_base *base;
    bool fixed_step[FACE_COUNT];
    bool introduced_step[FACE_COUNT];
    struct extension_cursor cursor;
    unsigned applied = 0;
    unsigned depth;

    if (base_index >= search->shared->extension_bases.count ||
        new_owner >= DICE) {
        search->internal_error = true;
        atomic_store_explicit(&search->shared->internal_error, true,
                              memory_order_relaxed);
        atomic_store_explicit(&search->shared->stop, true,
                              memory_order_relaxed);
        return false;
    }
    base = &search->shared->extension_bases.items[base_index];
    initialize_extension_cursor(base, new_owner, &cursor);
    for (depth = 0; depth < prefixes->depth; ++depth) {
        uint8_t step = prefix[depth];
        enum search_end end = (step & PREFIX_RIGHT_FLAG) != 0
            ? SEARCH_RIGHT : SEARCH_LEFT;
        unsigned owner = step & ~PREFIX_RIGHT_FLAG;
        struct end_plan plan = plan_extension_end(search, &cursor);

        if (owner >= DICE || end != plan.end ||
            (plan.owner_mask & (1U << owner)) == 0 ||
            !apply_extension_owner(
                search, &cursor, end, owner, &fixed_step[depth],
                &introduced_step[depth])) {
            search->internal_error = true;
            atomic_store_explicit(&search->shared->internal_error, true,
                                  memory_order_relaxed);
            atomic_store_explicit(&search->shared->stop, true,
                                  memory_order_relaxed);
            break;
        }
        ++applied;
    }
    if (!search->internal_error) {
        search_extensions(search, &cursor);
    }
    while (applied > 0) {
        uint8_t step;
        enum search_end end;
        unsigned owner;

        --applied;
        step = prefix[applied];
        end = (step & PREFIX_RIGHT_FLAG) != 0
            ? SEARCH_RIGHT : SEARCH_LEFT;
        owner = step & ~PREFIX_RIGHT_FLAG;
        undo_extension_owner(search, &cursor, end, owner,
                             fixed_step[applied],
                             introduced_step[applied]);
    }
    return !search_should_stop(search);
}

static bool run_extension_job(struct worker *worker, uint64_t job)
{
    uint64_t begin;
    uint64_t end;
    uint64_t prefix;

    job_prefix_range(worker->shared, job, &begin, &end);
    for (prefix = begin; prefix < end; ++prefix) {
        if (!run_extension_prefix(&worker->search, prefix)) {
            return false;
        }
    }
    return true;
}
#endif

static bool run_job(struct worker *worker, uint64_t job)
{
    uint64_t begin;
    uint64_t end;
    uint64_t prefix;

#if RESIDUAL_EXTENSION && !MIRROR && !COLUMN_GROUP
    if (worker->shared->options.extend_solutions_path != NULL) {
        return run_extension_job(worker, job);
    }
#endif
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
        .duplicate_configurations = atomic_load_explicit(
            &shared->duplicate_configurations, memory_order_relaxed),
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

        printf("permutation-fair #%" PRIu64
               " depth=%dd%d mirror=%d column-group=%d extension=%d "
               "encoding=%s\n",
               solution->number, DICE, SIDES, MIRROR, COLUMN_GROUP,
               shared->options.extend_solutions_path != NULL,
               solution->encoding);
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
    char nodes[SI_COUNT_TEXT_SIZE];
    char negative_prunes[SI_COUNT_TEXT_SIZE];
#if !MIRROR
    char right_end_prunes[SI_COUNT_TEXT_SIZE];
    char left_end_prunes[SI_COUNT_TEXT_SIZE];
#endif
    char configurations[SI_COUNT_TEXT_SIZE];
    char duplicate_configurations[SI_COUNT_TEXT_SIZE];

    format_si_count(nodes, totals.nodes);
    format_si_count(negative_prunes, totals.negative_prunes);
#if !MIRROR
    format_si_count(right_end_prunes, totals.right_end_prunes);
    format_si_count(left_end_prunes, totals.left_end_prunes);
#endif
    format_si_count(configurations, totals.configurations);
    format_si_count(duplicate_configurations,
                    totals.duplicate_configurations);

    fprintf(stderr,
            "%sprogress: %.1fs, workers=%u, jobs=%" PRIu64 "/%" PRIu64
            ", nodes=%s, negative-prunes=%s",
            terminal ? "\r" : "", monotonic_seconds() - start_time, active,
            jobs_done, shared->job_count, nodes, negative_prunes);
#if !MIRROR
    if (shared->options.extend_solutions_path == NULL) {
        fprintf(stderr, ", right-end-prunes=%s, left-end-prunes=%s",
                right_end_prunes, left_end_prunes);
    }
#endif
    fprintf(stderr, ", permutation-fair=%s%s", configurations,
            shared->options.extend_solutions_path != NULL ?
                ", duplicate-results=" : "");
    if (shared->options.extend_solutions_path != NULL) {
        fprintf(stderr, "%s", duplicate_configurations);
    }
    fputs(terminal ? "\033[K" : "\n", stderr);
    fflush(stderr);
}

static void watch_workers(struct shared_state *shared,
                          const struct worker *workers,
                          unsigned worker_count, double start_time)
{
    const bool terminal = isatty(STDERR_FILENO) != 0;
    const double progress_seconds =
        (double)shared->options.progress_seconds;
    double next_status = monotonic_seconds() + progress_seconds;
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
        if (progress_seconds != 0.0 && now >= next_status) {
            print_progress(shared, workers, worker_count, start_time,
                           terminal);
            printed_status = true;
            next_status = now + progress_seconds;
        }
        /* Wake at least once a second so Ctrl-C remains responsive even
         * when progress output is disabled or has a long interval. */
        wait_seconds = 1.0;
        if (progress_seconds != 0.0) {
            double until_status = next_status - monotonic_seconds();

            if (until_status < wait_seconds) {
                wait_seconds = until_status;
            }
        }
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
    if (options.extend_solutions_path != NULL && (MIRROR || COLUMN_GROUP)) {
        fputs("--extend-solutions requires MIRROR=0 and COLUMN_GROUP=0.\n",
              stderr);
        return EXIT_FAILURE;
    }
#if !RESIDUAL_EXTENSION
    if (options.extend_solutions_path != NULL) {
        fputs("This residual_search build does not include extension search; "
              "rebuild with RESIDUAL_EXTENSION=1.\n", stderr);
        return EXIT_FAILURE;
    }
#endif
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
        fputs("Search complete: 0.000s, nodes=0, negative-prunes=0", stderr);
#if !MIRROR
        if (options.extend_solutions_path == NULL) {
            fputs(", right-end-prunes=0, left-end-prunes=0", stderr);
        }
#endif
        fputs(", permutation-fair=0\n", stderr);
        return EXIT_SUCCESS;
    }
    for (owner = 0; owner < DICE; ++owner) {
        prototype.remaining[owner] = SIDES;
    }
    prototype.encoding[FACE_COUNT] = '\0';
    start_time = monotonic_seconds();
#if RESIDUAL_EXTENSION
    if (options.extend_solutions_path != NULL &&
        !load_extension_bases(options.extend_solutions_path, &prototype,
                              &shared.extension_bases)) {
        return interrupt_requested ? 128 + SIGINT : EXIT_FAILURE;
    }
#endif
    {
        bool prefixes_built;

#if RESIDUAL_EXTENSION && !MIRROR && !COLUMN_GROUP
        prefixes_built = options.extend_solutions_path != NULL
            ? build_extension_prefixes(
                  &prototype, &shared.extension_bases, &shared.prefixes,
                  desired_jobs)
            : build_prefixes(&prototype, &shared.prefixes, desired_jobs);
#else
        prefixes_built = build_prefixes(&prototype, &shared.prefixes,
                                        desired_jobs);
#endif
        if (!prefixes_built) {
            if (interrupt_requested) {
                fprintf(stderr,
                        "Search interrupted during prefix-job setup: %.3fs, "
                        "configuration=%dd%d, mirror=%d, column-group=%d, "
                        "order=%s",
                        monotonic_seconds() - start_time, DICE, SIDES,
                        MIRROR, COLUMN_GROUP,
                        options.random_order ? "random" : "canonical");
                if (options.random_order) {
                    fprintf(stderr, ", seed=%" PRIu64, options.seed);
                }
#if RESIDUAL_EXTENSION
                if (options.extend_solutions_path != NULL) {
                    fprintf(stderr, ", mode=extension, bases=%" PRIu64
                            ", positions-per-base=%u, input=%s",
                            shared.extension_bases.count,
                            DICE,
                            options.extend_solutions_path);
                }
#endif
                fputc('\n', stderr);
            } else {
                fputs("Unable to build residual-search prefix jobs.\n",
                      stderr);
            }
            clear_prefixes(&shared.prefixes);
            free_shared_extension_bases(&shared);
            return interrupt_requested ? 128 + SIGINT : EXIT_FAILURE;
        }
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
            clear_prefixes(&shared.prefixes);
            free_shared_extension_bases(&shared);
            return EXIT_FAILURE;
        }
        shared.job_order = malloc((size_t)shared.job_count *
                                  sizeof(*shared.job_order));
        if (shared.job_order == NULL) {
            fputs("Unable to allocate job-order table.\n", stderr);
            clear_prefixes(&shared.prefixes);
            free_shared_extension_bases(&shared);
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
    atomic_init(&shared.duplicate_configurations, 0);
    atomic_init(&shared.active_workers, 0);
    atomic_init(&shared.stop, false);
    atomic_init(&shared.internal_error, false);
    atomic_init(&shared.node_limit_reached, false);
    if (pthread_mutex_init(&shared.solution_mutex, NULL) != 0) {
        fputs("Unable to initialize solution mutex.\n", stderr);
        free(shared.job_order);
        clear_prefixes(&shared.prefixes);
        free_shared_extension_bases(&shared);
        return EXIT_FAILURE;
    }
    if (pthread_mutex_init(&shared.event_mutex, NULL) != 0) {
        fputs("Unable to initialize worker-event mutex.\n", stderr);
        pthread_mutex_destroy(&shared.solution_mutex);
        free(shared.job_order);
        clear_prefixes(&shared.prefixes);
        free_shared_extension_bases(&shared);
        return EXIT_FAILURE;
    }
    if (pthread_cond_init(&shared.event_condition, NULL) != 0) {
        fputs("Unable to initialize worker-event condition.\n", stderr);
        pthread_mutex_destroy(&shared.event_mutex);
        pthread_mutex_destroy(&shared.solution_mutex);
        free(shared.job_order);
        clear_prefixes(&shared.prefixes);
        free_shared_extension_bases(&shared);
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

#if RESIDUAL_EXTENSION
    if (options.extend_solutions_path != NULL) {
        fprintf(stderr,
                "Extending %" PRIu64 " canonical %u-die bases",
                shared.extension_bases.count, BASE_DICE);
        if (shared.extension_bases.minimum_sides ==
            shared.extension_bases.maximum_sides) {
            fprintf(stderr, " with %u sides",
                    shared.extension_bases.minimum_sides);
        } else {
            fprintf(stderr, " with %u-%u sides",
                    shared.extension_bases.minimum_sides,
                    shared.extension_bases.maximum_sides);
        }
        fprintf(stderr,
                " to %dd%d by residual extension search (%u states; "
                "two-end fail-first; %u new-die positions/base) with %u "
                "pthread workers (%" PRIu64
                " jobs over %" PRIu64 " prefixes, split depth %u moves; "
                "order=%s, input=%s",
                DICE, SIDES, PERM_STATE_COUNT, DICE, worker_count,
                shared.job_count, shared.prefixes.count,
                shared.prefixes.depth,
                options.random_order ? "random" : "canonical",
                options.extend_solutions_path);
        if (shared.extension_bases.duplicate_count != 0) {
            fprintf(stderr, ", duplicate-bases=%" PRIu64,
                    shared.extension_bases.duplicate_count);
        }
        if (shared.extension_bases.lesser_count != 0) {
            fprintf(stderr, ", lesser-records=%" PRIu64,
                    shared.extension_bases.lesser_count);
        }
        if (shared.extension_bases.same_side_nonfair_count != 0) {
            fprintf(stderr, ", same-side-nonfair-skipped=%" PRIu64,
                    shared.extension_bases.same_side_nonfair_count);
        }
    } else
#endif
    {
        fprintf(stderr,
                "Searching essentially different %s%s%dd%d configurations "
                "by residual-prefix search (%u states; %s; column-group=%d) "
                "with %u pthread workers (%" PRIu64 " jobs over %" PRIu64
                " prefixes, split depth %u %s; order=%s",
                MIRROR ? "mirrored " : "",
                COLUMN_GROUP ? "column-grouped " : "", DICE, SIDES,
                PERM_STATE_COUNT,
                MIRROR ? "forced mirror-pair peeling" :
                RESIDUAL_TWO_END_SEARCH ? "two-end fail-first enabled" :
                RESIDUAL_RIGHT_END_CHECK ? "right-end checks enabled" :
                                           "left-only peeling",
                COLUMN_GROUP,
                worker_count, shared.job_count, shared.prefixes.count,
                shared.prefixes.depth,
                MIRROR ? "mirrored pairs" : "faces",
                options.random_order ? "random" : "canonical");
    }
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
        char nodes[SI_COUNT_TEXT_SIZE];
        char negative_prunes[SI_COUNT_TEXT_SIZE];
#if !MIRROR
        char right_end_prunes[SI_COUNT_TEXT_SIZE];
        char left_end_prunes[SI_COUNT_TEXT_SIZE];
#endif
        char configurations[SI_COUNT_TEXT_SIZE];
        char duplicate_configurations[SI_COUNT_TEXT_SIZE];

        format_si_count(nodes, totals.nodes);
        format_si_count(negative_prunes, totals.negative_prunes);
#if !MIRROR
        format_si_count(right_end_prunes, totals.right_end_prunes);
        format_si_count(left_end_prunes, totals.left_end_prunes);
#endif
        format_si_count(configurations, totals.configurations);
        format_si_count(duplicate_configurations,
                        totals.duplicate_configurations);

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
                "/%" PRIu64 ", mirror=%d, column-group=%d, split-depth=%u, "
                "split-unit=%s, order=%s",
                status, monotonic_seconds() - start_time, DICE, SIDES,
                worker_count, jobs_done, shared.job_count,
                MIRROR, COLUMN_GROUP,
                shared.prefixes.depth,
                options.extend_solutions_path != NULL ? "moves" :
                (MIRROR ? "pairs" : "faces"),
                options.random_order ? "random" : "canonical");
        if (options.random_order) {
            fprintf(stderr, ", seed=%" PRIu64, options.seed);
        }
#if RESIDUAL_EXTENSION
        if (options.extend_solutions_path != NULL) {
            fprintf(stderr,
                    ", mode=extension, bases=%" PRIu64
                    ", positions-per-base=%u, input=%s",
                    shared.extension_bases.count,
                    DICE,
                    options.extend_solutions_path);
        }
#endif
        fprintf(stderr,
                ", nodes=%s, negative-prunes=%s",
                nodes, negative_prunes);
#if !MIRROR
        if (options.extend_solutions_path == NULL) {
            fprintf(stderr,
                    ", right-end-prunes=%s, left-end-prunes=%s",
                    right_end_prunes, left_end_prunes);
        }
#endif
        fprintf(stderr, ", permutation-fair=%s", configurations);
        if (options.extend_solutions_path != NULL) {
            fprintf(stderr, ", duplicate-results=%s",
                    duplicate_configurations);
        }
        fputc('\n', stderr);
    }
    if (interrupt_requested && exit_status == EXIT_SUCCESS) {
        exit_status = 128 + SIGINT;
    }

cleanup:
    drain_solutions(&shared);
    free(workers);
    free(shared.job_order);
    clear_prefixes(&shared.prefixes);
    free_shared_extension_bases(&shared);
    free_shared_solution_set(&shared);
    pthread_cond_destroy(&shared.event_condition);
    pthread_mutex_destroy(&shared.event_mutex);
    pthread_mutex_destroy(&shared.solution_mutex);
    return exit_status;
}
