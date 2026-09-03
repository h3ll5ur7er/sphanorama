#pragma once
#include <functional>
#include "sphanorama/types.h"

namespace sphanorama {

enum class EventKind : uint16_t {
  CoverageChanged, CellSatisfied, SessionEnded, BuildProgressed, BuildCompleted, NodeInvalidated
};

struct Event {
  EventKind kind{};
  SessionId session;
  BuildId build;
  NodeId node;
};

// The only sanctioned channel between managers (docs/03 §3.3 rule 3): publish, never call. A
// direct manager-to-manager call would couple two use-case sequences that are meant to vary
// independently.
class IEventBus {
 public:
  virtual ~IEventBus() = default;
  virtual void Publish(const Event&) = 0;
  virtual int64_t Subscribe(EventKind, std::function<void(const Event&)>) = 0;
  virtual void Unsubscribe(int64_t token) = 0;
};

}  // namespace sphanorama
