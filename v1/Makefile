# Everything, from the top.
#
#   make            build the core and run every check
#   make core       CMake build + the C++ unit tests
#   make editor     the TypeScript tests (needs the core built for the
#                   cross-check; it skips with a message if it is missing)
#   make bench      the section 11 performance budget, measured
#   make demo       compile the sample and explain a decision
#   make clean

NPM ?= npm
CMAKE ?= cmake

CORE_BUILD := core/build
VITEST := editor/node_modules/.bin/vitest
TSC := editor/node_modules/.bin/tsc

.PHONY: all check core editor typecheck bench demo deps clean

all: check

check: core editor
	@echo
	@echo "================================================================"
	@echo " everything passed"
	@echo "================================================================"

# ------------------------------------------------------------------- core

core:
	@echo "=== core: C++17, no engine dependency ========================="
	@$(CMAKE) -S core -B $(CORE_BUILD) -DCMAKE_BUILD_TYPE=Release > /dev/null
	@$(CMAKE) --build $(CORE_BUILD) -j 4
	@cd $(CORE_BUILD) && ctest --output-on-failure

# ----------------------------------------------------------------- editor

deps:
	@test -d editor/node_modules || (cd editor && $(NPM) install)

editor: deps
	@echo
	@echo "=== editor: format, validation, protocol, replay =============="
	@echo "=== plus the cross-check against the C++ core ================="
	@cd editor && ../$(VITEST) run

typecheck: deps
	@cd editor && ../$(TSC) --noEmit && echo "typecheck clean"

# ------------------------------------------------------------- tools

bench: core
	@echo
	@./$(CORE_BUILD)/aib_bench

# Compile the sample and print a score breakdown for one situation -- the
# section 8.2 view, on the command line.
demo: deps
	@echo "=== compiling the sample ======================================"
	@cd editor && $(NPM) exec --yes tsx src/cli/aibc.ts compile \
		../samples/guard.behavior.yaml -o ../samples/guard.aib
	@echo
	@echo "=== a wounded guard with a target closing ====================="
	@cd editor && $(NPM) exec --yes tsx src/cli/aibc.ts explain \
		../samples/guard.behavior.yaml \
		DistanceToTarget=800 Health=0.22 AmmoRatio=0.8 \
		TimeSinceLastSeen=2 AllyCount=0

clean:
	@rm -rf $(CORE_BUILD) editor/node_modules/.vite samples/guard.aib
	@rm -rf editor/.vitest editor/node_modules/.cache
	@find . -name '*.aib' -not -path './node_modules/*' -delete 2>/dev/null || true
