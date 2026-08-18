CC ?= cc
PGO_CC ?= gcc

DICE ?= 4
SIDES ?= 12
MIRROR ?= 0
HIGH_FIRST ?= 1
PERM_ONLY ?= 0
PERM_LINEAR ?= 1
LINEAR_STRIDE ?= 2
PERM_LINEAR_STRIDE ?= 3
RESIDUAL_RIGHT_END ?= 1
RESIDUAL_TWO_END ?= 1
PGO_SECONDS ?= 5
PGO_RUN_ARGS ?= --quiet --threads 1 --jobs 256 --random-order --seed 1

CPPFLAGS += -DDICE=$(DICE) -DSIDES=$(SIDES) -DMIRROR=$(MIRROR) \
	-DHIGH_FIRST=$(HIGH_FIRST) -DPERM_ONLY=$(PERM_ONLY) \
	-DADDITIVE_PERM_LINEAR=$(PERM_LINEAR) \
	-DLINEAR_BOUND_STRIDE=$(LINEAR_STRIDE) \
	-DADDITIVE_PERM_LINEAR_BOUND_STRIDE=$(PERM_LINEAR_STRIDE)
CFLAGS ?= -O3 -march=native -flto
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -pthread

RESIDUAL_CPPFLAGS = -DRESIDUAL_RIGHT_END_CHECK=$(RESIDUAL_RIGHT_END) \
	-DRESIDUAL_TWO_END_SEARCH=$(RESIDUAL_TWO_END)

all: column_search weave_search residual_search

pgo: pgo-column_search

permutation_search: override PERM_ONLY = 1

column_search permutation_search: column_search.c FORCE
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

weave_search: weave_search.c FORCE
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

residual_search: residual_search.c FORCE
	$(CC) $(CPPFLAGS) $(RESIDUAL_CPPFLAGS) \
		$(CFLAGS) -o $@ $< $(LDLIBS)

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
	echo "Building instrumented $(1)" \
		"(DICE=$(DICE), SIDES=$(SIDES), PERM_ONLY=$(PERM_ONLY))"; \
	$(PGO_CC) $(CPPFLAGS) $(3) $(CFLAGS) \
		-fprofile-generate="$$pgo_dir" -fprofile-update=atomic \
		-o $(1) $(2) $(LDLIBS); \
	echo "Training $(1) for up to $(PGO_SECONDS) seconds"; \
	status=0; \
	timeout --signal=INT --kill-after=2s $(PGO_SECONDS)s \
		./$(1) $(PGO_RUN_ARGS) || status=$$?; \
	case $$status in \
		0|124|130) ;; \
		*) echo "PGO training failed with status $$status" >&2; exit $$status ;; \
	esac; \
	echo "Rebuilding $(1) with the collected profile"; \
	$(PGO_CC) $(CPPFLAGS) $(3) $(CFLAGS) \
		-fprofile-use="$$pgo_dir" -fprofile-correction \
		-fprofile-partial-training -Wno-missing-profile \
		-o $(1) $(2) $(LDLIBS)
endef

pgo-column_search: column_search.c FORCE
	$(call build_with_pgo,column_search,column_search.c)

pgo-permutation_search: override PERM_ONLY = 1
pgo-permutation_search: column_search.c FORCE
	$(call build_with_pgo,permutation_search,column_search.c)

pgo-residual_search: residual_search.c FORCE
	$(call build_with_pgo,residual_search,residual_search.c,$(RESIDUAL_CPPFLAGS))

.PHONY: all pgo pgo-column_search pgo-permutation_search \
	pgo-residual_search FORCE clean
FORCE:

clean:
	rm -f column_search permutation_search weave_search residual_search
