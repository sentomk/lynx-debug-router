#include "lynx_debug_router/request_tracker.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

namespace {

using namespace lynx::debug_router;

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
                       AttachmentId attachment, std::int64_t original_id = 1) {
  const auto result = tracker.Track(peer, attachment, original_id);
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
                  std::int64_t original_id = 1) {
  CHECK(request.backend_id == backend_id);
  CHECK(request.attachment.id == attachment);
  CHECK(request.attachment.peer == peer);
  CHECK(request.attachment.target == target);
  CHECK(request.original_id == original_id);
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
    CheckFailure(f.tracker.Track(f.a, f.aa, 1), RequestStatus::kInvalidLimits);
    CHECK(f.tracker.PendingCount() == 0);
    CHECK(!f.tracker.Complete(BackendRequestId::kInvalid));
  }
}

void RegistrationValidation() {
  Fixture f;
  CheckFailure(f.tracker.Track(PeerId::kInvalid, f.aa, 1),
               RequestStatus::kPeerNotFound);
  CheckFailure(f.tracker.Track(f.a, AttachmentId::kInvalid, 1),
               RequestStatus::kAttachmentNotFound);
  CheckFailure(f.tracker.Track(f.b, f.aa, 1), RequestStatus::kNotOwner);
  CHECK(f.registry.Detach(f.a, f.aa).status == SessionStatus::kOk);
  CheckFailure(f.tracker.Track(f.a, f.aa, 1),
               RequestStatus::kAttachmentNotFound);
  CHECK(f.registry.RemovePeer(f.a).status == SessionStatus::kOk);
  CheckFailure(f.tracker.Track(f.a, f.aa, 1), RequestStatus::kPeerNotFound);
  CHECK(f.tracker.PendingCount() == 0);
  CHECK(Track(f.tracker, f.b, f.bb) == static_cast<BackendRequestId>(1));
}

void DuplicateRequest() {
  Fixture f({1, 10});
  const auto first = Track(f.tracker, f.a, f.aa, 7);
  CheckFailure(f.tracker.Track(f.a, f.aa, 7), RequestStatus::kDuplicateRequest);
  CheckFailure(f.tracker.Track(f.a, f.aa, 8), RequestStatus::kCapacityExceeded);
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
  CheckFailure(f.tracker.Track(f.a, f.aa, 2),
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
  CheckFailure(f.tracker.Track(f.a, f.aa, 2), RequestStatus::kCapacityExceeded);
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
  CheckFailure(f.tracker.Track(f.a, f.aa, 2), RequestStatus::kIdExhausted);
  CHECK(f.tracker.PendingCount() == 2);
  CHECK(f.tracker.Complete(first).has_value());
  CHECK(f.tracker.Complete(second).has_value());
  CheckFailure(f.tracker.Track(f.a, f.aa, 1), RequestStatus::kIdExhausted);
  CHECK(f.tracker.PendingCount() == 0);
}

void InvalidationNeverReusesIds() {
  Fixture f({8, 1});
  const auto id = Track(f.tracker, f.a, f.aa);
  CHECK(f.tracker.Invalidate(f.registry.Detach(f.a, f.aa)).size() == 1);
  const auto replacement = Attach(f.registry, f.a, f.target);
  CheckFailure(f.tracker.Track(f.a, replacement, 1),
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
