#include "lynx_debug_router/request_tracker.h"

#include <algorithm>

namespace lynx::debug_router {

RequestTracker::RequestTracker(SessionRegistry& registry,
                               RequestTrackerLimits limits)
    : registry_(registry), limits_(limits) {}

TrackResult RequestTracker::Track(PeerId peer, AttachmentId attachment,
                                  std::int64_t original_id) {
  if (limits_.max_pending == 0 || limits_.max_backend_id <= 0) {
    return {RequestStatus::kInvalidLimits};
  }
  if (!registry_.HasPeer(peer)) {
    return {RequestStatus::kPeerNotFound};
  }
  const auto current = registry_.FindAttachment(attachment);
  if (!current) {
    return {RequestStatus::kAttachmentNotFound};
  }
  if (current->peer != peer) {
    return {RequestStatus::kNotOwner};
  }
  for (const auto& [id, request] : pending_) {
    if (request.attachment.id == attachment &&
        request.original_id == original_id) {
      return {RequestStatus::kDuplicateRequest};
    }
  }
  if (pending_.size() >= limits_.max_pending) {
    return {RequestStatus::kCapacityExceeded};
  }
  if (next_backend_id_ > limits_.max_backend_id) {
    return {RequestStatus::kIdExhausted};
  }

  const auto id = static_cast<BackendRequestId>(next_backend_id_);
  pending_.emplace(id, PendingRequest{id, *current, original_id});
  ++next_backend_id_;
  return {RequestStatus::kOk, id};
}

std::optional<PendingRequest> RequestTracker::Complete(BackendRequestId id) {
  const auto found = pending_.find(id);
  if (found == pending_.end()) {
    return std::nullopt;
  }
  const auto request = found->second;
  pending_.erase(found);
  const auto current = registry_.FindAttachment(request.attachment.id);
  if (!current || current->peer != request.attachment.peer ||
      current->target != request.attachment.target) {
    return std::nullopt;
  }
  return request;
}

std::vector<PendingRequest> RequestTracker::Invalidate(
    const CloseResult& closure) {
  std::vector<PendingRequest> removed;
  if (closure.status != SessionStatus::kOk || closure.closed.empty()) {
    return removed;
  }
  // Collect snapshots before mutation so allocation failure cannot partially
  // invalidate pending work when the consumer enables C++ exceptions.
  for (const auto& entry : pending_) {
    const auto& request = entry.second;
    const bool matches = std::any_of(
        closure.closed.begin(), closure.closed.end(),
        [&request](const ClosedAttachment& closed) {
          return closed.attachment.id == request.attachment.id &&
                 closed.attachment.peer == request.attachment.peer &&
                 closed.attachment.target == request.attachment.target;
        });
    if (matches && !registry_.FindAttachment(request.attachment.id)) {
      removed.push_back(request);
    }
  }
  for (const auto& request : removed) {
    pending_.erase(request.backend_id);
  }
  return removed;
}

std::size_t RequestTracker::PendingCount() const { return pending_.size(); }

}  // namespace lynx::debug_router
