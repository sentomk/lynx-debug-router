#ifndef LYNX_DEBUG_ROUTER_REQUEST_TRACKER_H_
#define LYNX_DEBUG_ROUTER_REQUEST_TRACKER_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "lynx_debug_router/session_registry.h"

namespace lynx::debug_router {

enum class BackendRequestId : std::int32_t { kInvalid = 0 };

struct RequestTrackerLimits {
  std::size_t max_pending = 0;
  // Inclusive positive ceiling, chosen for the complete backend path. Zero or
  // negative ceilings and a zero pending limit reject all Track operations.
  std::int32_t max_backend_id = 0;
};

enum class RequestStatus {
  kOk,
  kInvalidLimits,
  kPeerNotFound,
  kAttachmentNotFound,
  kNotOwner,
  kDuplicateRequest,
  kCapacityExceeded,
  kIdExhausted,
};

struct TrackResult {
  RequestStatus status;
  BackendRequestId id = BackendRequestId::kInvalid;
};

struct PendingRequest {
  BackendRequestId backend_id;
  Attachment attachment;
  std::int64_t original_id;
};

// One tracker owns the ID space of one shared backend channel, potentially
// spanning several targets. Do not use independent trackers for peers/targets
// whose responses arrive on the same channel. IDs are never reused by a
// tracker. Its lifetime must cover that channel's possible responses: replacing
// a tracker is safe only after the old backend response source has been
// retired/fenced.
//
// The registry must outlive this object. Serialize all calls with registry
// calls. There is no I/O, JSON parsing, callback, executor, timer, or backend
// cancellation.
class RequestTracker final {
 public:
  RequestTracker(SessionRegistry& registry, RequestTrackerLimits limits);
  RequestTracker(const RequestTracker&) = delete;
  RequestTracker& operator=(const RequestTracker&) = delete;
  RequestTracker(RequestTracker&&) = delete;
  RequestTracker& operator=(RequestTracker&&) = delete;

  // peer comes from trusted connection context. Requires a live, owned
  // attachment and rejects duplicate original IDs pending on that attachment.
  // The protocol adapter validates the frontend ID's wire representation.
  [[nodiscard]] TrackResult Track(PeerId peer, AttachmentId attachment,
                                  std::int64_t original_id);

  // Consumes one pending response and returns a route snapshot only while the
  // attachment is still live. Unknown, duplicate, and stale responses return
  // nullopt, never a broadcast destination. Use the result in the same serial
  // turn, or revalidate it before deferred delivery.
  [[nodiscard]] std::optional<PendingRequest> Complete(BackendRequestId id);

  // Consume a closure result from the bound registry. Returns removed requests
  // in backend-ID order, after state is updated. Failed/repeated closures are
  // harmless; snapshots of attachments that are still live are ignored.
  [[nodiscard]] std::vector<PendingRequest> Invalidate(
      const CloseResult& closure);

  // Includes stale records until Invalidate or Complete removes them. The owner
  // must promptly consume registry closures to release their capacity.
  [[nodiscard]] std::size_t PendingCount() const;

 private:
  const SessionRegistry& registry_;
  const RequestTrackerLimits limits_;
  // Wider than the wire ID so incrementing INT32_MAX cannot overflow.
  std::int64_t next_backend_id_ = 1;
  std::map<BackendRequestId, PendingRequest> pending_;
};

}  // namespace lynx::debug_router

#endif  // LYNX_DEBUG_ROUTER_REQUEST_TRACKER_H_
