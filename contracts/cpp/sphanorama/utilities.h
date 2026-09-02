// The utilities bar: available to every layer, depends on nothing above it.
#pragma once

#include <functional>
#include <span>
#include <string_view>
#include "types.h"

namespace sphanorama {

enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error };

class ILogger {
 public:
  virtual ~ILogger() = default;
  virtual void Log(LogLevel, const char* component, std::string_view message) = 0;
};

class IClock {
 public:
  virtual ~IClock() = default;
  virtual int64_t MonotonicNs() = 0;
  virtual int64_t WallMs() = 0;
};

// Feature flags and tunables. Algorithm parameters live here, not in constants, so the bench
// client and A/B tuning can sweep them without a rebuild.
class IConfigStore {
 public:
  virtual ~IConfigStore() = default;
  virtual bool Flag(std::string_view key, bool fallback) = 0;
  virtual double Number(std::string_view key, double fallback) = 0;
  virtual std::string_view Text(std::string_view key, std::string_view fallback) = 0;
};

// Bump-allocated scratch for per-stage work. Engines allocate here, never with new/malloc,
// so peak usage is measurable and bounded.
class IArena {
 public:
  virtual ~IArena() = default;
  virtual std::span<uint8_t> Take(int64_t bytes, int32_t alignment) = 0;
  virtual void ResetTo(int64_t mark) = 0;
  virtual int64_t Mark() const = 0;
};

// Timings and counters, kept on-device. Feeds the bench reports and the in-app diagnostics panel.
class IDiagnosticsSink {
 public:
  virtual ~IDiagnosticsSink() = default;
  virtual void Timing(const char* component, const char* stage, double milliseconds) = 0;
  virtual void Counter(const char* component, const char* name, int64_t delta) = 0;
};

// The only sanctioned cross-manager channel (docs/03 §3.3 rule 3): publish, never call.
enum class EventKind : uint16_t {
  CoverageChanged, CellSatisfied, SessionEnded, BuildProgressed, BuildCompleted, NodeInvalidated
};

struct Event {
  EventKind kind;
  SessionId session;
  BuildId build;
  NodeId node;
};

class IEventBus {
 public:
  virtual ~IEventBus() = default;
  virtual void Publish(const Event&) = 0;
  virtual int64_t Subscribe(EventKind, std::function<void(const Event&)>) = 0;
  virtual void Unsubscribe(int64_t token) = 0;
};

}  // namespace sphanorama
