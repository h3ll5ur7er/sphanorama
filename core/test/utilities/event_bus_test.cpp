// The bus is the only sanctioned channel between managers (docs/03 §3.3 rule 3), so its delivery
// semantics are part of the architecture rather than an implementation detail. In particular a
// handler that unsubscribes during dispatch must not then be called: managers tear down mid-event.
#include <string>
#include <gtest/gtest.h>

#include <vector>

#include "utilities/event_bus.h"

namespace sphanorama {
namespace {

Event Ev(EventKind kind, uint64_t node = 0) {
  Event e;
  e.kind = kind;
  e.node = NodeId{node};
  return e;
}

TEST(EventBus, DeliversToASubscriberOfThatKind) {
  EventBus bus;
  int seen = 0;
  bus.Subscribe(EventKind::CellSatisfied, [&](const Event&) { ++seen; });
  bus.Publish(Ev(EventKind::CellSatisfied));
  EXPECT_EQ(seen, 1);
}

TEST(EventBus, DoesNotDeliverOtherKinds) {
  EventBus bus;
  int seen = 0;
  bus.Subscribe(EventKind::CellSatisfied, [&](const Event&) { ++seen; });
  bus.Publish(Ev(EventKind::BuildCompleted));
  EXPECT_EQ(seen, 0);
}

TEST(EventBus, DeliversToEverySubscriberOfThatKind) {
  EventBus bus;
  int a = 0, b = 0;
  bus.Subscribe(EventKind::CoverageChanged, [&](const Event&) { ++a; });
  bus.Subscribe(EventKind::CoverageChanged, [&](const Event&) { ++b; });
  bus.Publish(Ev(EventKind::CoverageChanged));
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 1);
}

TEST(EventBus, PassesThePublishedEventThrough) {
  EventBus bus;
  NodeId got{};
  bus.Subscribe(EventKind::NodeInvalidated, [&](const Event& e) { got = e.node; });
  bus.Publish(Ev(EventKind::NodeInvalidated, 42));
  EXPECT_EQ(got.value, 42u);
}

TEST(EventBus, UnsubscribeStopsDelivery) {
  EventBus bus;
  int seen = 0;
  const int64_t token = bus.Subscribe(EventKind::SessionEnded, [&](const Event&) { ++seen; });
  bus.Unsubscribe(token);
  bus.Publish(Ev(EventKind::SessionEnded));
  EXPECT_EQ(seen, 0);
}

TEST(EventBus, UnsubscribingAnUnknownTokenIsHarmless) {
  EventBus bus;
  bus.Unsubscribe(9999);
  int seen = 0;
  bus.Subscribe(EventKind::SessionEnded, [&](const Event&) { ++seen; });
  bus.Publish(Ev(EventKind::SessionEnded));
  EXPECT_EQ(seen, 1);
}

TEST(EventBus, TokensAreNotReusedAfterUnsubscribe) {
  // Otherwise a stale token from one component could silently cancel another's subscription.
  EventBus bus;
  const int64_t first = bus.Subscribe(EventKind::SessionEnded, [](const Event&) {});
  bus.Unsubscribe(first);
  const int64_t second = bus.Subscribe(EventKind::SessionEnded, [](const Event&) {});
  EXPECT_NE(first, second);
}

TEST(EventBus, AHandlerThatUnsubscribesAnotherDuringDispatchCancelsIt) {
  EventBus bus;
  int late = 0;
  int64_t late_token = 0;
  bus.Subscribe(EventKind::BuildProgressed, [&](const Event&) { bus.Unsubscribe(late_token); });
  late_token = bus.Subscribe(EventKind::BuildProgressed, [&](const Event&) { ++late; });
  bus.Publish(Ev(EventKind::BuildProgressed));
  EXPECT_EQ(late, 0);
}

TEST(EventBus, SubscribingDuringDispatchDoesNotReceiveTheInFlightEvent) {
  EventBus bus;
  int added = 0;
  bus.Subscribe(EventKind::BuildProgressed, [&](const Event&) {
    bus.Subscribe(EventKind::BuildProgressed, [&](const Event&) { ++added; });
  });
  bus.Publish(Ev(EventKind::BuildProgressed));
  EXPECT_EQ(added, 0);
  bus.Publish(Ev(EventKind::BuildProgressed));
  EXPECT_EQ(added, 1);
}

TEST(EventBus, PublishingFromAHandlerIsDelivered) {
  EventBus bus;
  int inner = 0;
  bus.Subscribe(EventKind::CellSatisfied,
                [&](const Event&) { bus.Publish(Ev(EventKind::CoverageChanged)); });
  bus.Subscribe(EventKind::CoverageChanged, [&](const Event&) { ++inner; });
  bus.Publish(Ev(EventKind::CellSatisfied));
  EXPECT_EQ(inner, 1);
}


TEST(EventBus, AHandlerThatSubscribesCanStillUseItsOwnCapturesAfterwards) {
  // The dispatcher held an iterator into the subscription vector and invoked the handler through
  // it. A handler that subscribes makes that vector reallocate, which moves and destroys the very
  // callable being executed — and for a small lambda, whose captures std::function stores inline,
  // those captures then live in the freed buffer. Every read after the subscribe is a
  // use-after-free.
  //
  // Two pointer captures on purpose: any more and libstdc++ puts the target on the heap, where a
  // move steals the pointer and the bug hides. The bug is real either way; this is the shape that
  // makes it observable.
  EventBus bus;
  int seen = 0;

  bus.Subscribe(EventKind::CoverageChanged, [&bus, &seen](const Event&) {
    for (int i = 0; i < 64; ++i) {
      bus.Subscribe(EventKind::CellSatisfied, [](const Event&) {});
    }
    ++seen;      // reading a capture after the vector has grown underneath us
  });

  bus.Publish(Ev(EventKind::CoverageChanged));
  EXPECT_EQ(seen, 1);
}

}  // namespace
}  // namespace sphanorama
