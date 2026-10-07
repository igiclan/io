#include <doctest/doctest.h>

#include "Models/SessionState.hpp"

using namespace pf;
using S = SessionState;

TEST_CASE("happy path and reconnect cycle") {
    SessionStateMachine sm;
    int notified = 0;
    sm.set_listener([&](const SessionTransition&) { ++notified; });
    CHECK(sm.transition(S::Authenticating));
    CHECK(sm.transition(S::Attaching));
    CHECK(sm.transition(S::Active));
    CHECK(sm.transition(S::Suspended, "network lost"));
    CHECK(sm.transition(S::Active, "resumed"));
    CHECK(sm.transition(S::Reconnecting, "mode switch"));
    CHECK(sm.transition(S::Active));
    CHECK(sm.transition(S::Detached));
    CHECK(sm.transition(S::Closed));
    CHECK(notified == 9);
    CHECK(sm.history().back().to == S::Closed);
}

TEST_CASE("illegal transitions are rejected without changing state") {
    SessionStateMachine sm;
    CHECK_FALSE(sm.transition(S::Active));
    CHECK(sm.state() == S::Created);
    CHECK(sm.transition(S::Failed));
    CHECK_FALSE(sm.transition(S::Active));
    CHECK(sm.transition(S::Closed));
    CHECK_FALSE(sm.transition(S::Created));
    CHECK_FALSE(sm.transition(S::Closed));
}

TEST_CASE("transition table is total and closed is terminal") {
    for (int f = 0; f <= static_cast<int>(S::Closed); ++f) {
        for (int t = 0; t <= static_cast<int>(S::Closed); ++t) {
            auto from = static_cast<S>(f), to = static_cast<S>(t);
            if (from == S::Closed) CHECK_FALSE(is_transition_allowed(from, to));
            if (from != S::Closed && from != to && to == S::Closed) CHECK(is_transition_allowed(from, to));
        }
        S parsed;
        CHECK(parse_session_state(to_string(static_cast<S>(f)), parsed));
        CHECK(parsed == static_cast<S>(f));
    }
}
