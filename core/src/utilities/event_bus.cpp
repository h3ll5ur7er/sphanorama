#include "utilities/event_bus.h"

#include <algorithm>

namespace sphanorama {

int64_t EventBus::Subscribe(EventKind kind, std::function<void(const Event&)> handler) {
  const int64_t token = next_token_++;
  subscriptions_.push_back(Subscription{token, kind, std::move(handler), true});
  return token;
}

void EventBus::Unsubscribe(int64_t token) {
  for (auto& sub : subscriptions_) {
    if (sub.token == token) {
      sub.alive = false;
      break;
    }
  }
  CompactIfIdle();
}

void EventBus::Publish(const Event& event) {
  // Snapshot tokens rather than iterators or pointers: a handler may subscribe (reallocating the
  // vector) or unsubscribe (invalidating a handler) while we are still walking the list.
  std::vector<int64_t> targets;
  targets.reserve(subscriptions_.size());
  for (const auto& sub : subscriptions_) {
    if (sub.alive && sub.kind == event.kind) targets.push_back(sub.token);
  }

  ++dispatch_depth_;
  for (const int64_t token : targets) {
    const auto it = std::find_if(subscriptions_.begin(), subscriptions_.end(),
                                 [token](const Subscription& s) { return s.token == token; });
    // Re-check liveness on every iteration: an earlier handler may have cancelled this one.
    if (it != subscriptions_.end() && it->alive && it->handler) it->handler(event);
  }
  --dispatch_depth_;

  CompactIfIdle();
}

void EventBus::CompactIfIdle() {
  if (dispatch_depth_ != 0) return;
  subscriptions_.erase(
      std::remove_if(subscriptions_.begin(), subscriptions_.end(),
                     [](const Subscription& s) { return !s.alive; }),
      subscriptions_.end());
}

}  // namespace sphanorama
