#include <stdbool.h>

#include "testkit.h"

bool should_print_report(long elapsed_ms, bool dirty);

UnitTest(should_not_print_before_interval) {
    tk_assert(!should_print_report(99, true),
              "report should stay throttled before 100ms");
}

UnitTest(should_print_at_interval_when_dirty) {
    tk_assert(should_print_report(100, true),
              "report should be emitted once 100ms has elapsed");
}
