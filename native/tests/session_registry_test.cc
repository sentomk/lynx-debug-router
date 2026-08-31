#include "lynx_debug_router/session_registry.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

namespace {

using namespace lynx::debug_router;

// Unlike assert(), checks stay active in Release and Android builds.
#define CHECK(condition)                                                \
  do {                                                                  \
    if (!(condition)) {                                                 \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; \
      std::exit(EXIT_FAILURE);                                          \
    }                                                                   \
  } while (false)

static_assert(!std::is_convertible_v<PeerId, TargetId>);
static_assert(!std::is_convertible_v<TargetId, AttachmentId>);
static_assert(!std::is_copy_constructible_v<SessionRegistry>);
static_assert(!std::is_move_constructible_v<SessionRegistry>);

AttachmentId Attach(SessionRegistry& registry, PeerId peer, TargetId target) {
  const auto result = registry.Attach(peer, target);
  CHECK(result.status == SessionStatus::kOk);
  CHECK(result.id != AttachmentId::kInvalid);
  const auto attachment = registry.FindAttachment(result.id);
  CHECK(attachment.has_value());
  CHECK(attachment->id == result.id);
  CHECK(attachment->peer == peer);
  CHECK(attachment->target == target);
  return result.id;
}

void CheckClosed(const ClosedAttachment& closed, AttachmentId id, PeerId peer,
                 TargetId target, AttachmentCloseReason reason) {
  CHECK(closed.attachment.id == id);
  CHECK(closed.attachment.peer == peer);
  CHECK(closed.attachment.target == target);
  CHECK(closed.reason == reason);
}

void Registration() {
  SessionRegistry registry;
  CHECK(!registry.HasPeer(PeerId::kInvalid));
  CHECK(!registry.HasTarget(TargetId::kInvalid));
  CHECK(!registry.FindAttachment(AttachmentId::kInvalid));
  const auto first_peer = registry.RegisterPeer();
  const auto second_peer = registry.RegisterPeer();
  const auto first_target = registry.RegisterTarget();
  const auto second_target = registry.RegisterTarget();
  CHECK(first_peer != PeerId::kInvalid);
  CHECK(first_peer != second_peer);
  CHECK(first_target != TargetId::kInvalid);
  CHECK(first_target != second_target);
  CHECK(registry.HasPeer(first_peer));
  CHECK(registry.HasPeer(second_peer));
  CHECK(registry.HasTarget(first_target));
  CHECK(registry.HasTarget(second_target));
  CHECK(registry.AttachmentsForPeer(first_peer).empty());
  CHECK(registry.AttachmentsForTarget(first_target).empty());
}

void AttachValidation() {
  SessionRegistry registry;
  const auto peer = registry.RegisterPeer();
  const auto target = registry.RegisterTarget();
  const auto bad_peer = registry.Attach(PeerId::kInvalid, target);
  CHECK(bad_peer.status == SessionStatus::kPeerNotFound);
  CHECK(bad_peer.id == AttachmentId::kInvalid);
  const auto bad_target = registry.Attach(peer, TargetId::kInvalid);
  CHECK(bad_target.status == SessionStatus::kTargetNotFound);
  CHECK(bad_target.id == AttachmentId::kInvalid);
  CHECK(registry.AttachmentsForPeer(peer).empty());
  CHECK(registry.AttachmentsForTarget(target).empty());
}

void DuplicateAttach() {
  SessionRegistry registry;
  const auto peer = registry.RegisterPeer();
  const auto target = registry.RegisterTarget();
  const auto first = Attach(registry, peer, target);
  CHECK(Attach(registry, peer, target) == first);
  CHECK(registry.AttachmentsForPeer(peer).size() == 1);
  CHECK(registry.AttachmentsForTarget(target).size() == 1);
}

void DetachOwnership() {
  SessionRegistry registry;
  const auto owner = registry.RegisterPeer();
  const auto other = registry.RegisterPeer();
  const auto target = registry.RegisterTarget();
  const auto id = Attach(registry, owner, target);
  const auto wrong_owner = registry.Detach(other, id);
  CHECK(wrong_owner.status == SessionStatus::kNotOwner);
  CHECK(wrong_owner.closed.empty());
  const auto unknown_peer = registry.Detach(PeerId::kInvalid, id);
  CHECK(unknown_peer.status == SessionStatus::kPeerNotFound);
  CHECK(unknown_peer.closed.empty());
  const auto unknown_attachment =
      registry.Detach(owner, AttachmentId::kInvalid);
  CHECK(unknown_attachment.status == SessionStatus::kAttachmentNotFound);
  CHECK(unknown_attachment.closed.empty());
  CHECK(registry.FindAttachment(id).has_value());
  CHECK(registry.AttachmentsForTarget(target).size() == 1);
}

void DetachAndReattach() {
  SessionRegistry registry;
  const auto peer = registry.RegisterPeer();
  const auto target = registry.RegisterTarget();
  const auto other_target = registry.RegisterTarget();
  const auto id = Attach(registry, peer, target);
  const auto other_id = Attach(registry, peer, other_target);
  const auto result = registry.Detach(peer, id);
  CHECK(result.status == SessionStatus::kOk);
  CHECK(result.closed.size() == 1);
  CheckClosed(result.closed[0], id, peer, target,
              AttachmentCloseReason::kDetached);
  CHECK(!registry.FindAttachment(id));
  CHECK(registry.FindAttachment(other_id).has_value());
  CHECK(registry.HasPeer(peer));
  CHECK(registry.HasTarget(target));
  CHECK(registry.AttachmentsForTarget(target).empty());
  const auto replacement = Attach(registry, peer, target);
  CHECK(replacement != id);
  const auto repeated = registry.Detach(peer, id);
  CHECK(repeated.status == SessionStatus::kAttachmentNotFound);
  CHECK(repeated.closed.empty());
  CHECK(registry.FindAttachment(replacement).has_value());
}

void PeerRemoval() {
  SessionRegistry registry;
  const auto a = registry.RegisterPeer();
  const auto b = registry.RegisterPeer();
  const auto first = registry.RegisterTarget();
  const auto second = registry.RegisterTarget();
  const auto a_first = Attach(registry, a, first);
  const auto b_first = Attach(registry, b, first);
  const auto a_second = Attach(registry, a, second);
  const auto result = registry.RemovePeer(a);
  CHECK(result.status == SessionStatus::kOk);
  CHECK(result.closed.size() == 2);
  CheckClosed(result.closed[0], a_first, a, first,
              AttachmentCloseReason::kPeerRemoved);
  CheckClosed(result.closed[1], a_second, a, second,
              AttachmentCloseReason::kPeerRemoved);
  CHECK(!registry.HasPeer(a));
  CHECK(registry.HasPeer(b));
  CHECK(registry.HasTarget(first));
  CHECK(registry.HasTarget(second));
  CHECK(!registry.FindAttachment(a_first));
  CHECK(!registry.FindAttachment(a_second));
  CHECK(registry.FindAttachment(b_first).has_value());
  CHECK(registry.AttachmentsForPeer(a).empty());
  CHECK(registry.AttachmentsForTarget(first).size() == 1);
  CHECK(registry.AttachmentsForTarget(second).empty());
}

void TargetRemoval() {
  SessionRegistry registry;
  const auto a = registry.RegisterPeer();
  const auto b = registry.RegisterPeer();
  const auto first = registry.RegisterTarget();
  const auto second = registry.RegisterTarget();
  const auto a_first = Attach(registry, a, first);
  const auto b_first = Attach(registry, b, first);
  const auto a_second = Attach(registry, a, second);
  const auto result = registry.RemoveTarget(first);
  CHECK(result.status == SessionStatus::kOk);
  CHECK(result.closed.size() == 2);
  CheckClosed(result.closed[0], a_first, a, first,
              AttachmentCloseReason::kTargetRemoved);
  CheckClosed(result.closed[1], b_first, b, first,
              AttachmentCloseReason::kTargetRemoved);
  CHECK(!registry.HasTarget(first));
  CHECK(registry.HasTarget(second));
  CHECK(registry.HasPeer(a));
  CHECK(registry.HasPeer(b));
  CHECK(!registry.FindAttachment(a_first));
  CHECK(!registry.FindAttachment(b_first));
  CHECK(registry.FindAttachment(a_second).has_value());
  CHECK(registry.AttachmentsForTarget(first).empty());
  CHECK(registry.AttachmentsForPeer(a).size() == 1);
  CHECK(registry.AttachmentsForPeer(b).empty());
}

void UnknownRemoval() {
  SessionRegistry registry;
  const auto peer = registry.RegisterPeer();
  const auto target = registry.RegisterTarget();
  const auto peer_removed = registry.RemovePeer(peer);
  CHECK(peer_removed.status == SessionStatus::kOk);
  CHECK(peer_removed.closed.empty());
  const auto target_removed = registry.RemoveTarget(target);
  CHECK(target_removed.status == SessionStatus::kOk);
  CHECK(target_removed.closed.empty());
  const auto repeated_peer = registry.RemovePeer(peer);
  CHECK(repeated_peer.status == SessionStatus::kPeerNotFound);
  CHECK(repeated_peer.closed.empty());
  const auto repeated_target = registry.RemoveTarget(target);
  CHECK(repeated_target.status == SessionStatus::kTargetNotFound);
  CHECK(repeated_target.closed.empty());
  CHECK(registry.RemovePeer(PeerId::kInvalid).status ==
        SessionStatus::kPeerNotFound);
  CHECK(registry.RemoveTarget(TargetId::kInvalid).status ==
        SessionStatus::kTargetNotFound);
}

void PeerReplacement() {
  SessionRegistry registry;
  const auto old_peer = registry.RegisterPeer();
  const auto target = registry.RegisterTarget();
  const auto old_attachment = Attach(registry, old_peer, target);
  CHECK(registry.RemovePeer(old_peer).status == SessionStatus::kOk);
  const auto replacement = registry.RegisterPeer();
  CHECK(replacement != old_peer);
  const auto new_attachment = Attach(registry, replacement, target);
  CHECK(new_attachment != old_attachment);
  CHECK(registry.Attach(old_peer, target).status ==
        SessionStatus::kPeerNotFound);
  CHECK(registry.RemovePeer(old_peer).status == SessionStatus::kPeerNotFound);
  CHECK(registry.Detach(replacement, old_attachment).status ==
        SessionStatus::kAttachmentNotFound);
  CHECK(registry.Detach(old_peer, new_attachment).status ==
        SessionStatus::kPeerNotFound);
  CHECK(registry.FindAttachment(new_attachment).has_value());
}

void TargetReplacement() {
  SessionRegistry registry;
  const auto peer = registry.RegisterPeer();
  const auto old_target = registry.RegisterTarget();
  const auto old_attachment = Attach(registry, peer, old_target);
  CHECK(registry.RemoveTarget(old_target).status == SessionStatus::kOk);
  const auto replacement = registry.RegisterTarget();
  CHECK(replacement != old_target);
  const auto new_attachment = Attach(registry, peer, replacement);
  CHECK(new_attachment != old_attachment);
  CHECK(registry.Attach(peer, old_target).status ==
        SessionStatus::kTargetNotFound);
  CHECK(registry.RemoveTarget(old_target).status ==
        SessionStatus::kTargetNotFound);
  CHECK(!registry.FindAttachment(old_attachment));
  CHECK(registry.FindAttachment(new_attachment).has_value());
}

void SnapshotQueries() {
  SessionRegistry registry;
  const auto peer = registry.RegisterPeer();
  const auto first = registry.RegisterTarget();
  const auto second = registry.RegisterTarget();
  const auto first_id = Attach(registry, peer, first);
  const auto second_id = Attach(registry, peer, second);
  auto snapshots = registry.AttachmentsForPeer(peer);
  CHECK(snapshots.size() == 2);
  CHECK(snapshots[0].id == first_id);
  CHECK(snapshots[1].id == second_id);
  snapshots[0].peer = PeerId::kInvalid;
  auto found = registry.FindAttachment(first_id);
  CHECK(found->peer == peer);
  found->target = TargetId::kInvalid;
  CHECK(registry.FindAttachment(first_id)->target == first);
  CHECK(registry.RemovePeer(peer).closed.size() == 2);
  CHECK(snapshots.size() == 2);
  CHECK(snapshots[1].id == second_id);
  CHECK(registry.AttachmentsForPeer(peer).empty());
  CHECK(registry.AttachmentsForPeer(PeerId::kInvalid).empty());
  CHECK(registry.AttachmentsForTarget(TargetId::kInvalid).empty());
}

void SharedTargetLifecycle() {
  SessionRegistry registry;
  const auto a = registry.RegisterPeer();
  const auto b = registry.RegisterPeer();
  const auto target = registry.RegisterTarget();
  const auto a_id = Attach(registry, a, target);
  const auto b_id = Attach(registry, b, target);
  CHECK(a_id != b_id);
  CHECK(registry.RemovePeer(a).closed.size() == 1);
  CHECK(registry.FindAttachment(b_id).has_value());
  const auto closed = registry.RemoveTarget(target);
  CHECK(closed.closed.size() == 1);
  CheckClosed(closed.closed[0], b_id, b, target,
              AttachmentCloseReason::kTargetRemoved);
  CHECK(registry.HasPeer(b));
  CHECK(!registry.FindAttachment(b_id));
  CHECK(registry.Detach(b, b_id).status == SessionStatus::kAttachmentNotFound);
}

void RelationshipConsistency() {
  SessionRegistry registry;
  std::vector<PeerId> peers;
  std::vector<TargetId> targets;
  std::vector<Attachment> attachments;
  for (int i = 0; i < 8; ++i) {
    peers.push_back(registry.RegisterPeer());
    targets.push_back(registry.RegisterTarget());
  }
  for (const auto peer : peers) {
    for (const auto target : targets) {
      attachments.push_back({Attach(registry, peer, target), peer, target});
    }
  }
  const auto check_consistency = [&] {
    for (const auto& attachment : attachments) {
      const bool expected = registry.HasPeer(attachment.peer) &&
                            registry.HasTarget(attachment.target);
      CHECK(registry.FindAttachment(attachment.id).has_value() == expected);
    }
    for (const auto peer : peers) {
      std::size_t expected = 0;
      for (const auto target : targets) {
        expected += registry.HasPeer(peer) && registry.HasTarget(target);
      }
      CHECK(registry.AttachmentsForPeer(peer).size() == expected);
    }
    for (const auto target : targets) {
      std::size_t expected = 0;
      for (const auto peer : peers) {
        expected += registry.HasPeer(peer) && registry.HasTarget(target);
      }
      CHECK(registry.AttachmentsForTarget(target).size() == expected);
    }
  };
  check_consistency();
  for (std::size_t i = 0; i < peers.size(); ++i) {
    CHECK(registry.RemovePeer(peers[i]).closed.size() == targets.size() - i);
    check_consistency();
    CHECK(registry.RemoveTarget(targets[i]).closed.size() ==
          peers.size() - i - 1);
    check_consistency();
  }
}

struct TestCase {
  const char* name;
  void (*run)();
};

const TestCase kTests[] = {
    {"registration", Registration},
    {"attach_validation", AttachValidation},
    {"duplicate_attach", DuplicateAttach},
    {"detach_ownership", DetachOwnership},
    {"detach_and_reattach", DetachAndReattach},
    {"peer_removal", PeerRemoval},
    {"target_removal", TargetRemoval},
    {"unknown_removal", UnknownRemoval},
    {"peer_replacement", PeerReplacement},
    {"target_replacement", TargetReplacement},
    {"snapshot_queries", SnapshotQueries},
    {"shared_target_lifecycle", SharedTargetLifecycle},
    {"relationship_consistency", RelationshipConsistency},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc > 2) {
    std::cerr << "Usage: session_registry_test [case]\n";
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
