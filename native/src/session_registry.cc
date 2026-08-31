#include "lynx_debug_router/session_registry.h"

namespace lynx::debug_router {

std::uint64_t SessionRegistry::AllocateId() {
  if (next_id_ == 0) {
    return 0;
  }
  // Unsigned wrap leaves the allocator permanently exhausted, never recycling
  // an ID that a delayed operation could still hold.
  return next_id_++;
}

PeerId SessionRegistry::RegisterPeer() {
  const auto id = static_cast<PeerId>(AllocateId());
  if (id != PeerId::kInvalid) {
    peers_.insert(id);
  }
  return id;
}

TargetId SessionRegistry::RegisterTarget() {
  const auto id = static_cast<TargetId>(AllocateId());
  if (id != TargetId::kInvalid) {
    targets_.insert(id);
  }
  return id;
}

AttachResult SessionRegistry::Attach(PeerId peer, TargetId target) {
  if (!HasPeer(peer)) {
    return {SessionStatus::kPeerNotFound};
  }
  if (!HasTarget(target)) {
    return {SessionStatus::kTargetNotFound};
  }
  for (const auto& [id, attachment] : attachments_) {
    if (attachment.peer == peer && attachment.target == target) {
      return {SessionStatus::kOk, id};
    }
  }

  const auto id = static_cast<AttachmentId>(AllocateId());
  if (id == AttachmentId::kInvalid) {
    return {SessionStatus::kIdExhausted};
  }
  attachments_.emplace(id, Attachment{id, peer, target});
  return {SessionStatus::kOk, id};
}

CloseResult SessionRegistry::Detach(PeerId peer, AttachmentId attachment) {
  if (!HasPeer(peer)) {
    return {SessionStatus::kPeerNotFound, {}};
  }
  const auto found = attachments_.find(attachment);
  if (found == attachments_.end()) {
    return {SessionStatus::kAttachmentNotFound, {}};
  }
  if (found->second.peer != peer) {
    return {SessionStatus::kNotOwner, {}};
  }
  return CloseAttachments({found->second}, AttachmentCloseReason::kDetached);
}

CloseResult SessionRegistry::RemovePeer(PeerId peer) {
  if (!HasPeer(peer)) {
    return {SessionStatus::kPeerNotFound, {}};
  }
  auto result = CloseAttachments(AttachmentsForPeer(peer),
                                 AttachmentCloseReason::kPeerRemoved);
  peers_.erase(peer);
  return result;
}

CloseResult SessionRegistry::RemoveTarget(TargetId target) {
  if (!HasTarget(target)) {
    return {SessionStatus::kTargetNotFound, {}};
  }
  auto result = CloseAttachments(AttachmentsForTarget(target),
                                 AttachmentCloseReason::kTargetRemoved);
  targets_.erase(target);
  return result;
}

bool SessionRegistry::HasPeer(PeerId peer) const {
  return peers_.find(peer) != peers_.end();
}

bool SessionRegistry::HasTarget(TargetId target) const {
  return targets_.find(target) != targets_.end();
}

std::optional<Attachment> SessionRegistry::FindAttachment(
    AttachmentId id) const {
  const auto found = attachments_.find(id);
  if (found == attachments_.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<Attachment> SessionRegistry::AttachmentsForPeer(PeerId peer) const {
  std::vector<Attachment> result;
  for (const auto& [id, attachment] : attachments_) {
    if (attachment.peer == peer) {
      result.push_back(attachment);
    }
  }
  return result;
}

std::vector<Attachment> SessionRegistry::AttachmentsForTarget(
    TargetId target) const {
  std::vector<Attachment> result;
  for (const auto& [id, attachment] : attachments_) {
    if (attachment.target == target) {
      result.push_back(attachment);
    }
  }
  return result;
}

CloseResult SessionRegistry::CloseAttachments(
    const std::vector<Attachment>& attachments, AttachmentCloseReason reason) {
  CloseResult result{SessionStatus::kOk, {}};
  result.closed.reserve(attachments.size());
  // Prepare all output before mutation so allocation failure cannot leave a
  // partially applied removal when the consumer enables C++ exceptions.
  for (const auto& attachment : attachments) {
    result.closed.push_back({attachment, reason});
  }
  for (const auto& attachment : attachments) {
    attachments_.erase(attachment.id);
  }
  return result;
}

}  // namespace lynx::debug_router
