// Session state machine (see Docs/ARCHITECTURE.md §2.6).
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace pf {

enum class SessionState : std::uint8_t {
    Created = 0,
    Authenticating,
    Attaching,
    Active,
    Suspended,     // transport lost; device kept reserved for reconnect_timeout
    Reconnecting,  // device re-enumerating (mode switch) or transport re-establishing
    Detached,      // device detached; session record retained
    Failed,
    Closed,        // terminal
};

std::string_view to_string(SessionState s) noexcept;
bool parse_session_state(std::string_view s, SessionState& out) noexcept;
bool is_transition_allowed(SessionState from, SessionState to) noexcept;
inline bool is_terminal(SessionState s) noexcept { return s == SessionState::Closed; }

struct SessionTransition {
    SessionState from;
    SessionState to;
    std::int64_t at_unix_ms;
    std::string reason;
};

// Thread-safe. Rejects illegal transitions (returns false) instead of throwing so a
// late network event can never corrupt state.
class SessionStateMachine {
public:
    using Listener = std::function<void(const SessionTransition&)>;

    explicit SessionStateMachine(SessionState initial = SessionState::Created);

    bool transition(SessionState to, std::string reason = {});
    [[nodiscard]] SessionState state() const;
    [[nodiscard]] std::int64_t entered_at_ms() const;  // when current state was entered
    [[nodiscard]] std::deque<SessionTransition> history() const;
    void set_listener(Listener l);

    static constexpr std::size_t kHistoryLimit = 64;

private:
    mutable std::mutex m_;
    SessionState state_;
    std::int64_t entered_at_ms_;
    std::deque<SessionTransition> history_;
    Listener listener_;
};

}  // namespace pf
