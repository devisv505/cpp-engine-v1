#pragma once

#include <cstdio>

// Minimal harness: no framework, no dependency. Each suite is one executable
// whose exit code ctest reads -- non-zero means at least one check failed.
//
//     int main() { TestRun t("Vector2"); CHECK(t, "adds", (a + b).x == 3.0f); return t.Result(); }
namespace tests {

    class TestRun {
        public:
            explicit TestRun(const char* name) : m_name(name)
            {
                std::printf("%s\n", name);
            }

            void Check(const char* what, const bool ok)
            {
                std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
                if (!ok) {
                    ++m_failures;
                }
            }

            [[nodiscard]] int Result() const
            {
                if (m_failures == 0) {
                    std::printf("  all passed\n");
                    return 0;
                }
                std::printf("  %d FAILED\n", m_failures);
                return 1;
            }

        private:
            const char* m_name = "";
            int         m_failures = 0;
    };

} // namespace tests

// The condition doubles as its own description when none is given.
#define CHECK(run, what, cond) (run).Check((what), (cond))
