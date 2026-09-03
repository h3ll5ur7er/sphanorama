#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "sphanorama/utilities/event_bus.h"

namespace sphanorama {

// Delivery semantics, which are part of the architecture rather than an implementation detail
// because managers tear down and rewire mid-event:
//
//   - a handler subscribed during dispatch does not receive the in-flight event;
//   - a handler unsubscribed during dispatch is not called, even if dispatch already began;
//   - Publish is re-entrant: publishing from a handler is delivered immediately;
//   - tokens are never reused, so a stale token cannot cancel someone else's subscription.
class EventBus final : public IEventBus {
 public:
  void Publish(const Event&) override;
  int64_t Subscribe(EventKind, std::function<void(const Event&)>) override;
  void Unsubscribe(int64_t token) override;

 private:
  struct Subscription {
    int64_t token = 0;
    EventKind kind{};
    std::function<void(const Event&)> handler;
    bool alive = true;
  };

  void CompactIfIdle();

  std::vector<Subscription> subscriptions_;
  int64_t next_token_ = 1;
  int dispatch_depth_ = 0;
};

}  // namespace sphanorama
