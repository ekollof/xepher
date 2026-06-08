#pragma once

#include <doctest/doctest.h>

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <unistd.h>

namespace {

constexpr unsigned k_default_test_timeout_secs = 5;

std::atomic<bool> alarm_armed{false};
const char *current_test_name = nullptr;

[[nodiscard]] unsigned default_test_timeout_secs()
{
    const char *env = std::getenv("XEPHER_TEST_TIMEOUT_SECS");
    if (!env || !*env)
        return k_default_test_timeout_secs;

    unsigned value = k_default_test_timeout_secs;
    const auto [ptr, ec] = std::from_chars(env, env + std::strlen(env), value);
    if (ec != std::errc{} || ptr != env + std::strlen(env) || value == 0)
        return k_default_test_timeout_secs;
    return value;
}

void disarm_test_timeout()
{
    alarm(0);
    alarm_armed.store(false);
    current_test_name = nullptr;
}

void arm_test_timeout(double limit_secs)
{
    alarm(0);
    alarm_armed.store(false);

    if (limit_secs <= 0.0)
        limit_secs = static_cast<double>(default_test_timeout_secs());

    const auto limit = static_cast<unsigned>(limit_secs);
    if (limit == 0)
        return;

    alarm_armed.store(true);
    alarm(limit);
}

void timeout_signal_handler(int)
{
    if (!alarm_armed.load())
        return;

    std::fprintf(stderr, "FATAL ERROR: test case exceeded time limit");
    if (current_test_name)
        std::fprintf(stderr, " (%s)", current_test_name);
    std::fprintf(stderr, "\n");
    std::abort();
}

struct TimeoutListener : doctest::IReporter {
    explicit TimeoutListener(const doctest::ContextOptions &) {}

    void report_query(const doctest::QueryData &) override {}
    void test_run_start() override
    {
        struct sigaction sa{};
        sa.sa_handler = timeout_signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGALRM, &sa, nullptr);
    }
    void test_run_end(const doctest::TestRunStats &) override { disarm_test_timeout(); }

    void test_case_start(const doctest::TestCaseData &tc) override
    {
        current_test_name = tc.m_name;
        arm_test_timeout(tc.m_timeout);
    }
    void test_case_reenter(const doctest::TestCaseData &tc) override
    {
        current_test_name = tc.m_name;
        arm_test_timeout(tc.m_timeout);
    }
    void test_case_end(const doctest::CurrentTestCaseStats &) override { disarm_test_timeout(); }
    void test_case_exception(const doctest::TestCaseException &) override { disarm_test_timeout(); }
    void subcase_start(const doctest::SubcaseSignature &) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData &) override {}
    void log_message(const doctest::MessageData &) override {}
    void test_case_skipped(const doctest::TestCaseData &) override {}
};

}  // namespace

DOCTEST_REGISTER_LISTENER("timeout_listener", 0, TimeoutListener);