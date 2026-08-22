CC ?= cc
PGO_CC ?= gcc

DICE ?= 4
SIDES ?= 12
MIRROR ?= 0
# Pair the first/last K columns; leave empty so legacy MIRROR=1 still means
# full mirroring.
MIRROR_COLUMNS ?=
COLUMN_GROUP ?= 0
HIGH_FIRST ?= 1
PERM_ONLY ?= 0
PERM_LINEAR ?= 1
# Exact additive row joins: both late rows for 4 dice, final built row for 5.
ROW_MITM ?= 0
ROW1_MITM ?= $(if $(filter 4,$(DICE)),$(ROW_MITM),0)
# The conditioned identities are especially effective for the five-die
# search.  Four-die builds keep them opt-in because they cross over only at
# the largest side counts measured so far.
COMPLETION_BOUNDS ?= $(if $(filter 5,$(DICE)),$(ROW_MITM),0)
EARLY_COMPLETION ?= 1
C_COMPLETION ?= 0
# Replace recursive C construction with an exact ternary half-row join.
C_MITM ?= 0
# Require completed AB to be fair against the collapsed C/D/E residual die.
AB_RESIDUAL ?= 1
# Apply exact additive ABR suffix bounds while B is under construction.
ABR_BOUNDS ?= $(AB_RESIDUAL)
# Use ABR contribution spans to break place-planner ties for B columns.
ABR_ORDER ?= $(ABR_BOUNDS)
ABR_ORDER_PRIMARY ?= 0
# Apply coupled linear bounds to the six ABR permutation coordinates.
ABR_LINEAR ?= 0
ABR_LINEAR_STRIDE ?= 3
# Leave empty to start halfway through B; override with a search-column depth.
ABR_LINEAR_START ?=
# Check a five-direction basis by default; all 15 pairs remain experimental.
ABR_LINEAR_FULL ?= 0
# Require completed ABC to be fair against the collapsed D/E residual die.
ABC_RESIDUAL ?= 1
# Attribute exclusive per-row thread CPU time; disabled for zero hot-loop cost.
TRACK_TIME ?= 0
# Direction one is scalar and cheap; directions three through six introduce
# successively more expensive sparse nonlinear frontiers.
C_COMPLETION_DIRECTIONS ?= 1
# Add the second scalar identity and their X+Y/X-Y support bounds.  This
# requires C_COMPLETION=1; meaningless combinations are compile-time errors.
C_COMPLETION_COUPLED ?= 0
LINEAR_STRIDE ?= 2
PERM_LINEAR_STRIDE ?= 3
RESIDUAL_RIGHT_END ?= 1
RESIDUAL_TWO_END ?= 1
RESIDUAL_EXTENSION ?= 1
PGO_SECONDS ?= 5
PGO_RUN_ARGS ?= --quiet --threads 1 --jobs 256 --random-order --seed 1

CPPFLAGS += -DDICE=$(DICE) -DSIDES=$(SIDES) -DMIRROR=$(MIRROR) \
	$(if $(strip $(MIRROR_COLUMNS)),-DMIRROR_COLUMNS=$(MIRROR_COLUMNS)) \
	-DHIGH_FIRST=$(HIGH_FIRST) -DPERM_ONLY=$(PERM_ONLY) \
	-DADDITIVE_PERM_LINEAR=$(PERM_LINEAR) \
	-DROW2_MITM=$(ROW_MITM) -DROW1_MITM=$(ROW1_MITM) \
	-DCONDITIONED_COMPLETION_BOUNDS=$(COMPLETION_BOUNDS) \
	-DEARLY_CONDITIONED_COMPLETION_BOUNDS=$(EARLY_COMPLETION) \
	-DINCREMENTAL_C_COMPLETION=$(C_COMPLETION) \
	-DC_MITM=$(C_MITM) \
	-DAB_RESIDUAL_PERM=$(AB_RESIDUAL) \
	-DAB_RESIDUAL_BOUNDS=$(ABR_BOUNDS) \
	-DAB_RESIDUAL_COLUMN_ORDER=$(ABR_ORDER) \
	-DAB_RESIDUAL_COLUMN_ORDER_PRIMARY=$(ABR_ORDER_PRIMARY) \
	-DAB_RESIDUAL_LINEAR_BOUNDS=$(ABR_LINEAR) \
	-DAB_RESIDUAL_LINEAR_BOUND_STRIDE=$(ABR_LINEAR_STRIDE) \
	$(if $(strip $(ABR_LINEAR_START)),-DAB_RESIDUAL_LINEAR_BOUND_START=$(ABR_LINEAR_START)) \
	-DAB_RESIDUAL_LINEAR_FULL_PAIRS=$(ABR_LINEAR_FULL) \
	-DABC_RESIDUAL_PERM=$(ABC_RESIDUAL) \
	-DTRACK_TIME=$(TRACK_TIME) \
	-DINCREMENTAL_C_COMPLETION_DIRECTIONS=$(C_COMPLETION_DIRECTIONS) \
	-DINCREMENTAL_C_COMPLETION_COUPLED=$(C_COMPLETION_COUPLED) \
	-DLINEAR_BOUND_STRIDE=$(LINEAR_STRIDE) \
	-DADDITIVE_PERM_LINEAR_BOUND_STRIDE=$(PERM_LINEAR_STRIDE)
CFLAGS ?= -O3 -march=native -flto=auto
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += -pthread

RESIDUAL_CPPFLAGS = -DCOLUMN_GROUP=$(COLUMN_GROUP) \
	-DRESIDUAL_RIGHT_END_CHECK=$(RESIDUAL_RIGHT_END) \
	-DRESIDUAL_TWO_END_SEARCH=$(RESIDUAL_TWO_END) \
	-DRESIDUAL_EXTENSION=$(RESIDUAL_EXTENSION)

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
		"(DICE=$(DICE), SIDES=$(SIDES), PERM_ONLY=$(PERM_ONLY)," \
		"ROW_MITM=$(ROW_MITM), C_MITM=$(C_MITM), AB_RESIDUAL=$(AB_RESIDUAL)/bounds$(ABR_BOUNDS)+order$(ABR_ORDER)/primary$(ABR_ORDER_PRIMARY)+linear$(ABR_LINEAR)/$(ABR_LINEAR_STRIDE)@$(if $(strip $(ABR_LINEAR_START)),$(ABR_LINEAR_START),half)+full$(ABR_LINEAR_FULL), ABC_RESIDUAL=$(ABC_RESIDUAL), TRACK_TIME=$(TRACK_TIME), C_COMPLETION=$(C_COMPLETION)/$(C_COMPLETION_DIRECTIONS)+coupled$(C_COMPLETION_COUPLED)," \
		"MIRROR_COLUMNS=$(if $(strip $(MIRROR_COLUMNS)),$(MIRROR_COLUMNS),legacy))"; \
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
	rm -rf __pycache__
