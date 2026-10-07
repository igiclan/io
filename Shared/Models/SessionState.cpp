#include "Models/SessionState.hpp"

#include "Common/Platform.hpp"

namespace pf {

std::string_view to_string(SessionState s) noexcept {
    switch (s) {
        case SessionState::Created: return "Created";
        case SessionState::Authenticating: return "Authenticating";
        case SessionState::Attaching: return "Attaching";
        case SessionState::Active: return "Active";
        case SessionState::Suspended: return "Suspended";
        case SessionState::Reconnecting: return "Reconnecting";
        case SessionState::Detached: return "Detached";
        case SessionState::Failed: return "Failed";
        case SessionState::Closed: return "Closed";
    }
    return "Invalid";
}

bool parse_session_state(std::string_view s, SessionState& out) noexcept {
    for (int i = 0; i <= static_cast<int>(SessionState::Closed); ++i) {
        auto st = static_cast<SessionState>(i);
        if (to_string(st) == s) {
            out = st;
            return true;
        }
    }
    return false;
}

bool is_transition_allowed(SessionState from, SessionState to) noexcept {
    using S = SessionState;
    if (from == to) return false;
    if (from == S::Closed) return false;
    if (to == S::Closed) return true;  // any live state may close
    switch (from) {
        case S::Created: return to == S::Authenticating || to == S::Attaching || to == S::Failed;
        case S::Authenticating: return to == S::Attaching || to == S::Failed;
        case S::Attaching: return to == S::Active || to == S::Failed || to == S::Detached;
        case S::Active:
            return to == S::Suspended || to == S::Reconnecting || to == S::Detached || to == S::Failed;
        case S::Suspended:
            return to == S::Active || to == S::Reconnecting || to == S::Detached || to == S::Failed;
        case S::Reconnecting:
            return to == S::Active || to == S::Suspended || to == S::Detached || to == S::Failed;
        case S::Detached: return to == S::Attaching;
        case S::Failed: return false;  // only Closed (handled above)
        case S::Closed: return false;
    }
    return false;
}

SessionStateMachine::SessionStateMachine(SessionState initial)
    : state_(initial), entered_at_ms_(unix_time_ms()) {}

bool SessionStateMachine::transition(SessionState to, std::string reason) {
    Listener listener;
    SessionTransition t;
    {
        std::lock_guard lk(m_);
        if (!is_transition_allowed(state_, to)) return false;
        t = SessionTransition{state_, to, unix_time_ms(), std::move(reason)};
        state_ = to;
        entered_at_ms_ = t.at_unix_ms;
        history_.push_back(t);
        if (history_.size() > kHistoryLimit) history_.pop_front();
        listener = listener_;
    }
    if (listener) listener(t);
    return true;
}

SessionState SessionStateMachine::state() const {
    std::lock_guard lk(m_);
    return state_;
}

std::int64_t SessionStateMachine::entered_at_ms() const {
    std::lock_guard lk(m_);
    return entered_at_ms_;
}

std::deque<SessionTransition> SessionStateMachine::history() const {
    std::lock_guard lk(m_);
    return history_;
}

void SessionStateMachine::set_listener(Listener l) {
    std::lock_guard lk(m_);
    listener_ = std::move(l);
}

}  // namespace pf
