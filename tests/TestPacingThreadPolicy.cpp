// SPDX-License-Identifier: MPL-2.0

#include <catch2/catch_test_macros.hpp>

#include "PacingThreadPolicy.h"

#include <chrono>
#include <thread>

using oxrsys::pacing_thread::Action;
using oxrsys::pacing_thread::Decide;
using oxrsys::pacing_thread::Promoted;

namespace
{

constexpr std::chrono::nanoseconds kPeriod72Hz{13888888};
constexpr std::chrono::nanoseconds kPeriod90Hz{11111111};

std::thread::id OtherThreadId()
{
    std::thread::id id;
    std::thread other([&] { id = std::this_thread::get_id(); });
    other.join();
    return id;
}

} // namespace

TEST_CASE("Pacing thread is left alone while realtime pacing is off", "[pacing-thread]")
{
    CHECK(Decide(Promoted{}, false, std::this_thread::get_id(), kPeriod72Hz) == Action::Keep);
}

TEST_CASE("Pacing thread is promoted on its first realtime wait", "[pacing-thread]")
{
    CHECK(Decide(Promoted{}, true, std::this_thread::get_id(), kPeriod72Hz) == Action::Promote);
}

TEST_CASE("Pacing thread keeps its policy while thread and period are unchanged", "[pacing-thread]")
{
    const Promoted promoted{std::this_thread::get_id(), kPeriod72Hz};
    CHECK(Decide(promoted, true, std::this_thread::get_id(), kPeriod72Hz) == Action::Keep);
}

TEST_CASE("Pacing thread is retuned when the refresh rate changes", "[pacing-thread]")
{
    const Promoted promoted{std::this_thread::get_id(), kPeriod72Hz};
    CHECK(Decide(promoted, true, std::this_thread::get_id(), kPeriod90Hz) == Action::Retune);
}

TEST_CASE("Pacing thread policy is restored when realtime pacing is switched off", "[pacing-thread]")
{
    const Promoted promoted{std::this_thread::get_id(), kPeriod72Hz};
    CHECK(Decide(promoted, false, std::this_thread::get_id(), kPeriod72Hz) == Action::Restore);
    CHECK(Decide(promoted, false, OtherThreadId(), kPeriod72Hz) == Action::Restore);
}

TEST_CASE("Pacing thread moves when another thread waits", "[pacing-thread]")
{
    const Promoted promoted{OtherThreadId(), kPeriod72Hz};
    CHECK(Decide(promoted, true, std::this_thread::get_id(), kPeriod72Hz) == Action::Move);
}
