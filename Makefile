# Builds both programs. `make test` runs the end-to-end tests;
# `make tsan` runs them again with ThreadSanitizer builds.

TSAN_CXXFLAGS = -std=c++17 -I../common -pthread -g -O1 -fsanitize=thread
TSAN_LDFLAGS = -lssl -lcrypto -fsanitize=thread

all:
	$(MAKE) -C tracker
	$(MAKE) -C client

test: all
	python3 tests/integration_test.py

# setarch -R turns off address randomisation, which recent kernels set too high for TSan.
# Run `make clean` afterwards to go back to normal builds.
tsan: clean
	rm -f tsan-report.*
	$(MAKE) CXXFLAGS="$(TSAN_CXXFLAGS)" LDFLAGS="$(TSAN_LDFLAGS)"
	TSAN_OPTIONS="log_path=$(CURDIR)/tsan-report" setarch $$(uname -m) -R python3 tests/integration_test.py
	@if ls tsan-report.* >/dev/null 2>&1; then cat tsan-report.*; echo "ThreadSanitizer reported problems"; exit 1; fi
	@echo "ThreadSanitizer: no reports"

clean:
	$(MAKE) -C tracker clean
	$(MAKE) -C client clean

.PHONY: all test tsan clean
