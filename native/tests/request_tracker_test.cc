#include "lynx_debug_router/request_tracker.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

namespace {

using namespace lynx::debug_router;

constexpr RequestTime At(std::int64_t milliseconds) {
  return RequestTime{} + std::chrono::milliseconds{milliseconds};
}

constexpr auto kDeadline = At(100);

#define CHECK(condition)                                                \
  do {                                                                  \
    if (!(condition)) {                                                 \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; \
      std::exit(EXIT_FAILURE);                                          \
    }                                                                   \
  } while (false)

static_assert(!std::is_copy_constructible_v<RequestTracker>);
static_assert(!std::is_move_constructible_v<RequestTracker>);
static_assert(!std::is_constructible_v<RequestTracker, SessionRegistry&&,
                                       RequestTrackerLimits>);
static_assert(!std::is_convertible_v<BackendRequestId, AttachmentId>);

AttachmentId Attach(SessionRegistry& registry, PeerId peer, TargetId target) {
  const auto result = registry.Attach(peer, target);
  CHECK(result.status == SessionStatus::kOk);
  return result.id;
}

BackendRequestId Track(RequestTracker& tracker, PeerId peer,
                       AttachmentId attachment, std::int64_t original_id = 1,
                       RequestTime deadline = kDeadline) {
  const auto result = tracker.Track(peer, attachment, original_id, deadline);
  CHECK(result.status == RequestStatus::kOk);
  CHECK(result.id != BackendRequestId::kInvalid);
  return result.id;
}

void CheckFailure(TrackResult result, RequestStatus expected) {
  CHECK(result.status == expected);
  CHECK(result.id == BackendRequestId::kInvalid);
}

void CheckRequest(const PendingRequest& request, BackendRequestId backend_id,
                  AttachmentId attachment, PeerId peer, TargetId target,
                  std::int64_t original_id = 1,
                  RequestTime deadline = kDeadline) {
  CHECK(request.backend_id == backend_id);
  CHECK(request.attachment.id == attachment);
  CHECK(request.attachment.peer == peer);
  CHECK(request.attachment.target == target);
  CHECK(request.original_id == original_id);
  CHECK(request.deadline == deadline);
}

struct Fixture {
  explicit Fixture(RequestTrackerLimits limits = {64, 1024})
      : tracker(registry, limits),
        a(registry.RegisterPeer()),
        b(registry.RegisterPeer()),
        target(registry.RegisterTarget()),
        aa(Attach(registry, a, target)),
        bb(Attach(registry, b, target)) {}

  SessionRegistry registry;
  RequestTracker tracker;
  PeerId a;
  PeerId b;
  TargetId target;
  AttachmentId aa;
  AttachmentId bb;
};

// Captures backend-visible IDs and lets a test control response order. No JSON,
// transport, timing, or frontend identity is available to the fake backend.
struct FakeBackend {
  void Send(BackendRequestId id) { received.push_back(id); }

  BackendRequestId RespondAt(std::size_t index) {
    CHECK(index < received.size());
    const auto id = received[index];
    received.erase(received.begin() + static_cast<std::ptrdiff_t>(index));
    return id;
  }

  std::vector<BackendRequestId> received;
};

void InvalidLimits() {
  for (const auto limits :
       {RequestTrackerLimits{}, RequestTrackerLimits{0, 1},
        RequestTrackerLimits{1, 0}, RequestTrackerLimits{1, -1}}) {
    Fixture f(limits);
    CheckFailure(f.tracker.Track(f.a, f.aa, 1, kDeadline),
                 RequestStatus::kInvalidLimits);
    CHECK(f.tracker.PendingCount() == 0);
    CHECK(!f.tracker.Complete(BackendRequestId::kInvalid));
    CHECK(!f.tracker.Abandon(BackendRequestId::kInvalid));
    CHECK(f.tracker.Expire(kDeadline).empty());
  }
}

void RegistrationValidation() {
  Fixture f;
  CheckFailure(f.tracker.Track(PeerId::kInvalid, f.aa, 1, kDeadline),
               RequestStatus::kPeerNotFound);
  CheckFailure(f.tracker.Track(f.a, AttachmentId::kInvalid, 1, kDeadline),
               RequestStatus::kAttachmentNotFound);
  CheckFailure(f.tracker.Track(f.b, f.aa, 1, kDeadline),
               RequestStatus::kNotOwner);
  CHECK(f.registry.Detach(f.a, f.aa).status == SessionStatus::kOk);
  CheckFailure(f.tracker.Track(f.a, f.aa, 1, kDeadline),
               RequestStatus::kAttachmentNotFound);
  CHECK(f.registry.RemovePeer(f.a).status == SessionStatus::kOk);
  CheckFailure(f.tracker.Track(f.a, f.aa, 1, kDeadline),
               RequestStatus::kPeerNotFound);
  CHECK(f.tracker.PendingCount() == 0);
  CHECK(Track(f.tracker, f.b, f.bb) == static_cast<BackendRequestId>(1));
}

void DuplicateRequest() {
  Fixture f({1, 10});
  const auto first = Track(f.tracker, f.a, f.aa, 7);
  CheckFailure(f.tracker.Track(f.a, f.aa, 7, kDeadline),
               RequestStatus::kDuplicateRequest);
  CheckFailure(f.tracker.Track(f.a, f.aa, 8, kDeadline),
               RequestStatus::kCapacityExceeded);
  CHECK(f.tracker.PendingCount() == 1);
  const auto response = f.tracker.Complete(first);
  CHECK(response);
  CheckRequest(*response, first, f.aa, f.a, f.target, 7);
  const auto replacement = Track(f.tracker, f.a, f.aa, 7);
  CHECK(replacement == static_cast<BackendRequestId>(2));
  CHECK(!f.tracker.Complete(first));
  CHECK(f.tracker.Complete(replacement).has_value());
}

void OutOfOrderResponses() {
  Fixture f;
  FakeBackend backend;
  const auto a = Track(f.tracker, f.a, f.aa);
  const auto b = Track(f.tracker, f.b, f.bb);
  CHECK(a != b);
  backend.Send(a);
  backend.Send(b);
  const auto b_response = f.tracker.Complete(backend.RespondAt(1));
  CHECK(b_response);
  CheckRequest(*b_response, b, f.bb, f.b, f.target);
  CHECK(f.tracker.PendingCount() == 1);
  const auto a_response = f.tracker.Complete(backend.RespondAt(0));
  CHECK(a_response);
  CheckRequest(*a_response, a, f.aa, f.a, f.target);
  CHECK(f.tracker.PendingCount() == 0);
}

void SharedBackendTargets() {
  Fixture f;
  const auto other_target = f.registry.RegisterTarget();
  const auto other_attachment = Attach(f.registry, f.a, other_target);
  const auto first = Track(f.tracker, f.a, f.aa);
  const auto second = Track(f.tracker, f.a, other_attachment);
  CHECK(first != second);
  const auto response = f.tracker.Complete(second);
  CHECK(response);
  CheckRequest(*response, second, other_attachment, f.a, other_target);
  const auto remaining = f.tracker.Complete(first);
  CHECK(remaining);
  CheckRequest(*remaining, first, f.aa, f.a, f.target);
}

void OneShotResponse() {
  Fixture f;
  const auto id = Track(f.tracker, f.a, f.aa);
  CHECK(!f.tracker.Complete(BackendRequestId::kInvalid));
  CHECK(!f.tracker.Complete(static_cast<BackendRequestId>(-1)));
  CHECK(!f.tracker.Complete(static_cast<BackendRequestId>(999)));
  CHECK(f.tracker.PendingCount() == 1);
  CHECK(f.tracker.Complete(id).has_value());
  CHECK(!f.tracker.Complete(id));
  CHECK(f.tracker.PendingCount() == 0);
}

void PeerInvalidation() {
  Fixture f;
  const auto other_target = f.registry.RegisterTarget();
  const auto other_attachment = Attach(f.registry, f.a, other_target);
  const auto a_first = Track(f.tracker, f.a, f.aa);
  const auto b = Track(f.tracker, f.b, f.bb);
  const auto a_second = Track(f.tracker, f.a, f.aa, 2);
  const auto a_other = Track(f.tracker, f.a, other_attachment);
  const auto closure = f.registry.RemovePeer(f.a);
  const auto removed = f.tracker.Invalidate(closure);
  CHECK(removed.size() == 3);
  CheckRequest(removed[0], a_first, f.aa, f.a, f.target);
  CheckRequest(removed[1], a_second, f.aa, f.a, f.target, 2);
  CheckRequest(removed[2], a_other, other_attachment, f.a, other_target);
  CHECK(f.tracker.PendingCount() == 1);
  CHECK(f.tracker.Invalidate(closure).empty());
  CHECK(!f.tracker.Complete(a_first));
  CHECK(!f.tracker.Complete(a_second));
  CHECK(!f.tracker.Complete(a_other));
  const auto response = f.tracker.Complete(b);
  CHECK(response);
  CheckRequest(*response, b, f.bb, f.b, f.target);
}

void TargetInvalidation() {
  Fixture f;
  const auto other_target = f.registry.RegisterTarget();
  const auto other_attachment = Attach(f.registry, f.a, other_target);
  const auto a = Track(f.tracker, f.a, f.aa);
  const auto other = Track(f.tracker, f.a, other_attachment);
  const auto b = Track(f.tracker, f.b, f.bb);
  const auto removed = f.tracker.Invalidate(f.registry.RemoveTarget(f.target));
  CHECK(removed.size() == 2);
  CheckRequest(removed[0], a, f.aa, f.a, f.target);
  CheckRequest(removed[1], b, f.bb, f.b, f.target);
  CHECK(f.tracker.PendingCount() == 1);
  CHECK(f.registry.HasPeer(f.a));
  CHECK(f.registry.HasPeer(f.b));
  const auto response = f.tracker.Complete(other);
  CHECK(response);
  CheckRequest(*response, other, other_attachment, f.a, other_target);
}

void DetachAndReattach() {
  Fixture f;
  const auto old_request = Track(f.tracker, f.a, f.aa);
  const auto closure = f.registry.Detach(f.a, f.aa);
  CHECK(f.tracker.Invalidate(closure).size() == 1);
  const auto replacement = Attach(f.registry, f.a, f.target);
  const auto new_request = Track(f.tracker, f.a, replacement);
  CHECK(new_request != old_request);
  CHECK(f.tracker.Invalidate(closure).empty());
  CHECK(!f.tracker.Complete(old_request));
  CheckFailure(f.tracker.Track(f.a, f.aa, 2, kDeadline),
               RequestStatus::kAttachmentNotFound);
  const auto response = f.tracker.Complete(new_request);
  CHECK(response);
  CheckRequest(*response, new_request, replacement, f.a, f.target);
}

void MissedInvalidation() {
  for (int operation = 0; operation < 3; ++operation) {
    Fixture f;
    const auto old_request = Track(f.tracker, f.a, f.aa);
    CloseResult closure{SessionStatus::kOk, {}};
    auto peer = f.a;
    auto target = f.target;
    if (operation == 0) {
      closure = f.registry.Detach(f.a, f.aa);
    } else if (operation == 1) {
      closure = f.registry.RemovePeer(f.a);
      peer = f.registry.RegisterPeer();
    } else {
      closure = f.registry.RemoveTarget(f.target);
      target = f.registry.RegisterTarget();
    }
    // Deliberately omit Invalidate until after the old response arrives.
    const auto attachment = Attach(f.registry, peer, target);
    const auto replacement = Track(f.tracker, peer, attachment);
    CHECK(f.tracker.PendingCount() == 2);
    CHECK(!f.tracker.Complete(old_request));
    CHECK(f.tracker.PendingCount() == 1);
    CHECK(f.tracker.Invalidate(closure).empty());
    const auto response = f.tracker.Complete(replacement);
    CHECK(response);
    CheckRequest(*response, replacement, attachment, peer, target);
  }
}

void InvalidClosure() {
  Fixture f;
  const auto id = Track(f.tracker, f.a, f.aa);
  CHECK(f.tracker.Invalidate(f.registry.Detach(f.b, f.aa)).empty());
  CHECK(f.tracker.Invalidate({SessionStatus::kOk, {}}).empty());
  const auto live = f.registry.FindAttachment(f.aa);
  CHECK(live);
  const CloseResult fabricated{SessionStatus::kOk,
                               {{*live, AttachmentCloseReason::kDetached}}};
  CHECK(f.tracker.Invalidate(fabricated).empty());
  const auto closure = f.registry.Detach(f.a, f.aa);
  auto failed = closure;
  failed.status = SessionStatus::kNotOwner;
  CHECK(f.tracker.Invalidate(failed).empty());
  auto mismatched = closure;
  mismatched.closed[0].attachment.peer = f.b;
  CHECK(f.tracker.Invalidate(mismatched).empty());
  CHECK(f.tracker.PendingCount() == 1);
  const auto removed = f.tracker.Invalidate(closure);
  CHECK(removed.size() == 1);
  CheckRequest(removed[0], id, f.aa, f.a, f.target);
}

void CapacityLimit() {
  Fixture f({2, 5});
  const auto first = Track(f.tracker, f.a, f.aa);
  const auto second = Track(f.tracker, f.b, f.bb);
  CheckFailure(f.tracker.Track(f.a, f.aa, 2, kDeadline),
               RequestStatus::kCapacityExceeded);
  CHECK(f.tracker.PendingCount() == 2);
  CHECK(f.tracker.Complete(first).has_value());
  const auto third = Track(f.tracker, f.a, f.aa, 2);
  CHECK(third == static_cast<BackendRequestId>(3));
  CHECK(f.tracker.Invalidate(f.registry.RemovePeer(f.b)).size() == 1);
  CHECK(!f.tracker.Complete(second));
  const auto fourth = Track(f.tracker, f.a, f.aa, 3);
  CHECK(fourth == static_cast<BackendRequestId>(4));
  CHECK(f.tracker.PendingCount() == 2);
}

void IdExhaustion() {
  Fixture f({3, 2});
  const auto first = Track(f.tracker, f.a, f.aa);
  const auto second = Track(f.tracker, f.b, f.bb);
  CHECK(first == static_cast<BackendRequestId>(1));
  CHECK(second == static_cast<BackendRequestId>(2));
  CheckFailure(f.tracker.Track(f.a, f.aa, 2, kDeadline),
               RequestStatus::kIdExhausted);
  CHECK(f.tracker.PendingCount() == 2);
  CHECK(f.tracker.Complete(first).has_value());
  CHECK(f.tracker.Complete(second).has_value());
  CheckFailure(f.tracker.Track(f.a, f.aa, 1, kDeadline),
               RequestStatus::kIdExhausted);
  CHECK(f.tracker.PendingCount() == 0);
}

void InvalidationNeverReusesIds() {
  Fixture f({8, 1});
  const auto id = Track(f.tracker, f.a, f.aa);
  CHECK(f.tracker.Invalidate(f.registry.Detach(f.a, f.aa)).size() == 1);
  const auto replacement = Attach(f.registry, f.a, f.target);
  CheckFailure(f.tracker.Track(f.a, replacement, 1, kDeadline),
               RequestStatus::kIdExhausted);
  CHECK(!f.tracker.Complete(id));
  CHECK(f.tracker.PendingCount() == 0);
}

void OriginalIdPreservation() {
  Fixture f({8, std::numeric_limits<std::int32_t>::max()});
  for (const std::int64_t original :
       {std::int64_t{0}, std::int64_t{-7},
        std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max()}) {
    const auto id = Track(f.tracker, f.a, f.aa, original);
    const auto response = f.tracker.Complete(id);
    CHECK(response);
    CheckRequest(*response, id, f.aa, f.a, f.target, original);
  }
}

void SharedTargetLifecycle() {
  Fixture f;
  FakeBackend backend;
  const auto a = Track(f.tracker, f.a, f.aa);
  const auto b = Track(f.tracker, f.b, f.bb);
  backend.Send(a);
  backend.Send(b);
  const auto first_response = f.tracker.Complete(backend.RespondAt(1));
  CHECK(first_response);
  CheckRequest(*first_response, b, f.bb, f.b, f.target);
  CHECK(f.tracker.Invalidate(f.registry.RemovePeer(f.a)).size() == 1);
  CHECK(!f.tracker.Complete(backend.RespondAt(0)));
  CHECK(f.registry.FindAttachment(f.bb).has_value());
  const auto next = Track(f.tracker, f.b, f.bb);
  backend.Send(next);
  const auto next_response = f.tracker.Complete(backend.RespondAt(0));
  CHECK(next_response);
  CheckRequest(*next_response, next, f.bb, f.b, f.target);
  CHECK(f.tracker.PendingCount() == 0);
}

void AbandonPending() {
  Fixture f;
  const auto first = Track(f.tracker, f.a, f.aa);
  const auto other = Track(f.tracker, f.a, f.aa, 2);
  const auto b = Track(f.tracker, f.b, f.bb);
  const auto ended = f.tracker.Abandon(first);
  CHECK(ended);
  CHECK(ended->reason == RequestEndReason::kAbandoned);
  CheckRequest(ended->request, first, f.aa, f.a, f.target);
  CHECK(f.tracker.PendingCount() == 2);
  CHECK(!f.tracker.Abandon(first));
  CHECK(!f.tracker.Complete(first));
  CHECK(f.registry.FindAttachment(f.aa).has_value());
  CHECK(f.registry.FindAttachment(f.bb).has_value());
  const auto other_response = f.tracker.Complete(other);
  CHECK(other_response);
  CheckRequest(*other_response, other, f.aa, f.a, f.target, 2);
  const auto b_response = f.tracker.Complete(b);
  CHECK(b_response);
  CheckRequest(*b_response, b, f.bb, f.b, f.target);
  CHECK(f.tracker.Expire(kDeadline).empty());
}

void AbandonUnknown() {
  Fixture f;
  CHECK(!f.tracker.Abandon(BackendRequestId::kInvalid));
  const auto completed = Track(f.tracker, f.a, f.aa);
  CHECK(!f.tracker.Abandon(static_cast<BackendRequestId>(-1)));
  CHECK(!f.tracker.Abandon(static_cast<BackendRequestId>(999)));
  CHECK(f.tracker.PendingCount() == 1);
  CHECK(f.tracker.Complete(completed).has_value());
  CHECK(!f.tracker.Abandon(completed));
  const auto expired = Track(f.tracker, f.a, f.aa);
  CHECK(f.tracker.Expire(kDeadline).size() == 1);
  CHECK(!f.tracker.Abandon(expired));
  const auto closed = Track(f.tracker, f.a, f.aa);
  CHECK(f.tracker.Invalidate(f.registry.Detach(f.a, f.aa)).size() == 1);
  CHECK(!f.tracker.Abandon(closed));
  CHECK(f.tracker.PendingCount() == 0);
}

void DeadlineBoundary() {
  Fixture f;
  const auto first = Track(f.tracker, f.a, f.aa, 1, At(20));
  const auto second = Track(f.tracker, f.b, f.bb, 1, At(10));
  const auto third = Track(f.tracker, f.a, f.aa, 2, At(20));
  const auto fourth = Track(f.tracker, f.b, f.bb, 2, At(30));
  CHECK(f.tracker.Expire(At(9)).empty());
  CHECK(f.tracker.PendingCount() == 4);
  const auto expired = f.tracker.Expire(At(20));
  CHECK(expired.size() == 3);
  // Output order follows backend IDs, not deadlines; equal deadlines all
  // expire.
  CheckRequest(expired[0].request, first, f.aa, f.a, f.target, 1, At(20));
  CheckRequest(expired[1].request, second, f.bb, f.b, f.target, 1, At(10));
  CheckRequest(expired[2].request, third, f.aa, f.a, f.target, 2, At(20));
  for (const auto& ended : expired) {
    CHECK(ended.reason == RequestEndReason::kTimedOut);
    CHECK(!f.tracker.Complete(ended.request.backend_id));
  }
  CHECK(f.tracker.PendingCount() == 1);
  CHECK(f.tracker.Expire(At(20)).empty());
  CHECK(f.tracker.Expire(At(29)).empty());
  const auto response = f.tracker.Complete(fourth);
  CHECK(response);
  CheckRequest(*response, fourth, f.bb, f.b, f.target, 2, At(30));
  CHECK(f.tracker.Expire(At(30)).empty());
}

void DeadlineExtremes() {
  Fixture f;
  const auto earliest = Track(f.tracker, f.a, f.aa, 1, RequestTime::min());
  const auto zero = Track(f.tracker, f.a, f.aa, 2, RequestTime{});
  const auto latest = Track(f.tracker, f.a, f.aa, 3, RequestTime::max());
  const auto at_min = f.tracker.Expire(RequestTime::min());
  CHECK(at_min.size() == 1);
  CheckRequest(at_min[0].request, earliest, f.aa, f.a, f.target, 1,
               RequestTime::min());
  const auto at_zero = f.tracker.Expire(RequestTime{});
  CHECK(at_zero.size() == 1);
  CheckRequest(at_zero[0].request, zero, f.aa, f.a, f.target, 2, RequestTime{});
  const auto at_max = f.tracker.Expire(RequestTime::max());
  CHECK(at_max.size() == 1);
  CheckRequest(at_max[0].request, latest, f.aa, f.a, f.target, 3,
               RequestTime::max());
  CHECK(f.tracker.PendingCount() == 0);
  // Track never reads time or implicitly expires a request with a past
  // deadline.
  const auto past = Track(f.tracker, f.a, f.aa, 1, At(-1));
  CHECK(f.tracker.PendingCount() == 1);
  const auto late_scan = f.tracker.Expire(RequestTime::max());
  CHECK(late_scan.size() == 1);
  CHECK(late_scan[0].request.backend_id == past);
  CHECK(f.tracker.Expire(RequestTime::max()).empty());
}

void ExpiryReleasesCapacity() {
  Fixture f({2, 10});
  FakeBackend backend;
  const auto old_a = Track(f.tracker, f.a, f.aa, 1, At(10));
  const auto b = Track(f.tracker, f.b, f.bb, 1, At(20));
  backend.Send(old_a);
  backend.Send(b);
  CheckFailure(f.tracker.Track(f.a, f.aa, 2, At(30)),
               RequestStatus::kCapacityExceeded);
  const auto expired = f.tracker.Expire(At(10));
  CHECK(expired.size() == 1);
  CHECK(expired[0].reason == RequestEndReason::kTimedOut);
  CheckRequest(expired[0].request, old_a, f.aa, f.a, f.target, 1, At(10));
  CHECK(f.tracker.PendingCount() == 1);
  // Same frontend ID, same attachment, new backend ID. This is a new caller
  // request, not an automatic retry of the potentially executed operation.
  const auto new_a = Track(f.tracker, f.a, f.aa, 1, At(30));
  CHECK(new_a == static_cast<BackendRequestId>(3));
  backend.Send(new_a);
  CHECK(!f.tracker.Complete(backend.RespondAt(0)));
  CHECK(f.tracker.PendingCount() == 2);
  const auto b_response = f.tracker.Complete(backend.RespondAt(0));
  CHECK(b_response);
  CheckRequest(*b_response, b, f.bb, f.b, f.target, 1, At(20));
  const auto a_response = f.tracker.Complete(backend.RespondAt(0));
  CHECK(a_response);
  CheckRequest(*a_response, new_a, f.aa, f.a, f.target, 1, At(30));
  CHECK(f.registry.FindAttachment(f.aa).has_value());
  CHECK(f.registry.FindAttachment(f.bb).has_value());
  CHECK(f.tracker.PendingCount() == 0);
}

void TerminalOrdering() {
  int operations[] = {0, 1, 2, 3};
  do {
    Fixture f;
    const auto id = Track(f.tracker, f.a, f.aa);
    std::size_t endings = 0;
    for (const auto operation : operations) {
      if (operation == 0) {
        const auto response = f.tracker.Complete(id);
        if (response) {
          CheckRequest(*response, id, f.aa, f.a, f.target);
          ++endings;
        }
      } else if (operation == 1) {
        const auto abandoned = f.tracker.Abandon(id);
        if (abandoned) {
          CHECK(abandoned->reason == RequestEndReason::kAbandoned);
          CheckRequest(abandoned->request, id, f.aa, f.a, f.target);
          ++endings;
        }
      } else if (operation == 2) {
        const auto expired = f.tracker.Expire(kDeadline);
        if (!expired.empty()) {
          CHECK(expired.size() == 1);
          CHECK(expired[0].reason == RequestEndReason::kTimedOut);
          CheckRequest(expired[0].request, id, f.aa, f.a, f.target);
          ++endings;
        }
      } else {
        const auto invalidated =
            f.tracker.Invalidate(f.registry.Detach(f.a, f.aa));
        if (!invalidated.empty()) {
          CHECK(invalidated.size() == 1);
          CheckRequest(invalidated[0], id, f.aa, f.a, f.target);
          ++endings;
        }
      }
      // The first operation consumes the request; none of the other three can.
      CHECK(endings == 1);
      CHECK(f.tracker.PendingCount() == 0);
    }
    CHECK(f.registry.FindAttachment(f.bb).has_value());
  } while (std::next_permutation(operations, operations + 4));
}

void ClosedAttachmentTermination() {
  for (int operation = 0; operation < 3; ++operation) {
    for (const bool expire : {false, true}) {
      Fixture f;
      const auto old = Track(f.tracker, f.a, f.aa);
      CloseResult closure{SessionStatus::kOk, {}};
      auto peer = f.a;
      auto target = f.target;
      if (operation == 0) {
        closure = f.registry.Detach(f.a, f.aa);
      } else if (operation == 1) {
        closure = f.registry.RemovePeer(f.a);
        peer = f.registry.RegisterPeer();
      } else {
        closure = f.registry.RemoveTarget(f.target);
        target = f.registry.RegisterTarget();
      }
      const auto attachment = Attach(f.registry, peer, target);
      const auto replacement = Track(f.tracker, peer, attachment, 1, At(200));
      // Deliberately miss invalidation. Both cleanup paths must report closure,
      // not a sendable timeout/abandonment for the replacement relationship.
      if (expire) {
        const auto ended = f.tracker.Expire(kDeadline);
        CHECK(ended.size() == 1);
        CHECK(ended[0].reason == RequestEndReason::kAttachmentClosed);
        CheckRequest(ended[0].request, old, f.aa, f.a, f.target);
      } else {
        const auto ended = f.tracker.Abandon(old);
        CHECK(ended);
        CHECK(ended->reason == RequestEndReason::kAttachmentClosed);
        CheckRequest(ended->request, old, f.aa, f.a, f.target);
      }
      CHECK(f.tracker.PendingCount() == 1);
      CHECK(f.tracker.Invalidate(closure).empty());
      CHECK(!f.tracker.Abandon(old));
      CHECK(!f.tracker.Complete(old));
      CHECK(f.tracker.Expire(kDeadline).empty());
      const auto response = f.tracker.Complete(replacement);
      CHECK(response);
      CheckRequest(*response, replacement, attachment, peer, target, 1,
                   At(200));
    }
  }
}

void TerminationNeverReusesIds() {
  for (const bool expire : {false, true}) {
    Fixture f({1, 1});
    const auto id = Track(f.tracker, f.a, f.aa);
    if (expire) {
      CHECK(f.tracker.Expire(kDeadline).size() == 1);
    } else {
      CHECK(f.tracker.Abandon(id).has_value());
    }
    CHECK(f.tracker.PendingCount() == 0);
    CheckFailure(f.tracker.Track(f.a, f.aa, 1, At(200)),
                 RequestStatus::kIdExhausted);
    CHECK(!f.tracker.Complete(id));
    CHECK(f.registry.FindAttachment(f.aa).has_value());
  }
}

void AbandonReleasesCapacity() {
  Fixture f({1, 10});
  const auto failed = Track(f.tracker, f.a, f.aa);
  CheckFailure(f.tracker.Track(f.b, f.bb, 1, kDeadline),
               RequestStatus::kCapacityExceeded);
  CHECK(f.tracker.Abandon(failed).has_value());
  CHECK(f.tracker.PendingCount() == 0);
  const auto replacement = Track(f.tracker, f.a, f.aa);
  CHECK(replacement == static_cast<BackendRequestId>(2));
  CHECK(!f.tracker.Complete(failed));
  const auto response = f.tracker.Complete(replacement);
  CHECK(response);
  CheckRequest(*response, replacement, f.aa, f.a, f.target);
  const auto b = Track(f.tracker, f.b, f.bb);
  CHECK(f.tracker.Complete(b).has_value());
}

void DeadlineSnapshot() {
  Fixture f;
  const auto completed = Track(f.tracker, f.a, f.aa, 1, At(50));
  // A rejected duplicate cannot reschedule the existing request.
  CheckFailure(f.tracker.Track(f.a, f.aa, 1, At(1)),
               RequestStatus::kDuplicateRequest);
  CHECK(f.tracker.Expire(At(1)).empty());
  const auto response = f.tracker.Complete(completed);
  CHECK(response);
  CheckRequest(*response, completed, f.aa, f.a, f.target, 1, At(50));
  const auto invalidated = Track(f.tracker, f.a, f.aa, 2, At(75));
  const auto removed = f.tracker.Invalidate(f.registry.Detach(f.a, f.aa));
  CHECK(removed.size() == 1);
  CheckRequest(removed[0], invalidated, f.aa, f.a, f.target, 2, At(75));
  CHECK(f.tracker.Expire(At(75)).empty());
}

struct TestCase {
  const char* name;
  void (*run)();
};

const TestCase kTests[] = {
    {"invalid_limits", InvalidLimits},
    {"registration_validation", RegistrationValidation},
    {"duplicate_request", DuplicateRequest},
    {"out_of_order_responses", OutOfOrderResponses},
    {"shared_backend_targets", SharedBackendTargets},
    {"one_shot_response", OneShotResponse},
    {"peer_invalidation", PeerInvalidation},
    {"target_invalidation", TargetInvalidation},
    {"detach_and_reattach", DetachAndReattach},
    {"missed_invalidation", MissedInvalidation},
    {"invalid_closure", InvalidClosure},
    {"capacity_limit", CapacityLimit},
    {"id_exhaustion", IdExhaustion},
    {"invalidation_never_reuses_ids", InvalidationNeverReusesIds},
    {"original_id_preservation", OriginalIdPreservation},
    {"shared_target_lifecycle", SharedTargetLifecycle},
    {"abandon_pending", AbandonPending},
    {"abandon_unknown", AbandonUnknown},
    {"deadline_boundary", DeadlineBoundary},
    {"deadline_extremes", DeadlineExtremes},
    {"expiry_releases_capacity", ExpiryReleasesCapacity},
    {"terminal_ordering", TerminalOrdering},
    {"closed_attachment_termination", ClosedAttachmentTermination},
    {"termination_never_reuses_ids", TerminationNeverReusesIds},
    {"abandon_releases_capacity", AbandonReleasesCapacity},
    {"deadline_snapshot", DeadlineSnapshot},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2) {
    std::cerr << "Usage: request_tracker_test [case]\n";
    return EXIT_FAILURE;
  }
  bool ran = false;
  for (const auto& test : kTests) {
    if (argc == 1 || std::string(argv[1]) == test.name) {
      test.run();
      std::cout << "PASS " << test.name << '\n';
      ran = true;
    }
  }
  if (!ran) {
    std::cerr << "Unknown test case: " << argv[1] << '\n';
  }
  return ran ? EXIT_SUCCESS : EXIT_FAILURE;
}
