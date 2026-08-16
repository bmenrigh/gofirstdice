CC ?= cc
PGO_CC ?= gcc

DICE ?= 4
SIDES ?= 12
MIRROR ?= 0
HIGH_FIRST ?= 1
PERM_ONLY ?= 0
LINEAR_STRIDE ?= 2
PGO_SECONDS ?= 5
PGO_RUN_ARGS ?= --quiet --threads 1 --jobs 256 --random-order --seed 1

CPPFLAGS += -DDICE=$(DICE) -DSIDES=$(SIDES) -DMIRROR=$(MIRROR) \
	-DHIGH_FIRST=$(HIGH_FIRST) -DPERM_ONLY=$(PERM_ONLY) \
	-DLINEAR_BOUND_STRIDE=$(LINEAR_STRIDE)
CFLAGS ?= -O3 -march=native -flto
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -pthread

all: column_search

pgo: pgo-column_search

permutation_search: override PERM_ONLY = 1

column_search permutation_search: column_search.c FORCE
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

# Build and train using the same output pathname that the profile-consuming
# build will use.  GCC incorporates that pathname into its profile filenames.
define build_with_pgo
	@set -eu; \
	pgo_dir=$$(mktemp -d /tmp/gofirstdice-pgo.XXXXXX); \
	case "$$pgo_dir" in \
		/tmp/gofirstdice-pgo.*) ;; \
		*) echo "Unexpected PGO directory: $$pgo_dir" >&2; exit 1 ;; \
	esac; \
	cleanup() { rm -rf "$${pgo_dir:?}"; }; \
	trap cleanup EXIT; \
	trap 'exit 130' HUP INT TERM; \
	echo "Building instrumented $(1) (PERM_ONLY=$(PERM_ONLY))"; \
	$(PGO_CC) $(CPPFLAGS) $(CFLAGS) \
		-fprofile-generate="$$pgo_dir" -fprofile-update=atomic \
		-o $(1) column_search.c $(LDLIBS); \
	echo "Training $(1) for up to $(PGO_SECONDS) seconds"; \
	status=0; \
	timeout --signal=INT --kill-after=2s $(PGO_SECONDS)s \
		./$(1) $(PGO_RUN_ARGS) || status=$$?; \
	case $$status in \
		0|124|130) ;; \
		*) echo "PGO training failed with status $$status" >&2; exit $$status ;; \
	esac; \
	echo "Rebuilding $(1) with the collected profile"; \
	$(PGO_CC) $(CPPFLAGS) $(CFLAGS) \
		-fprofile-use="$$pgo_dir" -fprofile-correction \
		-fprofile-partial-training -Wno-missing-profile \
		-o $(1) column_search.c $(LDLIBS)
endef

pgo-column_search: column_search.c FORCE
	$(call build_with_pgo,column_search)

pgo-permutation_search: override PERM_ONLY = 1
pgo-permutation_search: column_search.c FORCE
	$(call build_with_pgo,permutation_search)

.PHONY: all pgo pgo-column_search pgo-permutation_search FORCE clean
FORCE:

clean:
	rm -f column_search permutation_search
