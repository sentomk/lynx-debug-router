#ifndef LYNX_DEBUG_ROUTER_SESSION_REGISTRY_H_
#define LYNX_DEBUG_ROUTER_SESSION_REGISTRY_H_

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lynx::debug_router {

// IDs are issued by one registry, not supplied by transports or backends.
enum class PeerId : std::uint64_t { kInvalid = 0 };
enum class TargetId : std::uint64_t { kInvalid = 0 };
enum class AttachmentId : std::uint64_t { kInvalid = 0 };

// Opaque host-provided metadata captured when one target instance is
// registered. A template URL describes the instance but does not identify it;
// empty and duplicate values are valid. The registry does not parse the value.
struct TargetDescriptor {
  std::string template_url;
};

struct Target {
  TargetId id;
  TargetDescriptor descriptor;
};

enum class SessionStatus {
  kOk,
  kPeerNotFound,
  kTargetNotFound,
  kAttachmentNotFound,
  kNotOwner,
  kIdExhausted,
};

struct Attachment {
  AttachmentId id;
  PeerId peer;
  TargetId target;
};

struct AttachResult {
  SessionStatus status;
  AttachmentId id = AttachmentId::kInvalid;
};

enum class AttachmentCloseReason { kDetached, kPeerRemoved, kTargetRemoved };

struct ClosedAttachment {
  Attachment attachment;
  AttachmentCloseReason reason;
};

struct CloseResult {
  SessionStatus status;
  // Snapshots in ascending attachment-ID order. State is already updated when
  // returned; the caller decides how to notify clients and clean pending work.
  std::vector<ClosedAttachment> closed;
};

// In-memory relationship state only: no I/O, callbacks, threads, or CDP
// parsing. All calls (including reads) must be serialized by the owner. Handles
// are scoped to this registry's lifetime and must not cross registry instances.
// Drain queued work before destroying the registry. IDs are never reused within
// its lifetime, so replaced peers, targets, and attachments reject stale IDs.
class SessionRegistry final {
 public:
  SessionRegistry() = default;
  SessionRegistry(const SessionRegistry&) = delete;
  SessionRegistry& operator=(const SessionRegistry&) = delete;
  SessionRegistry(SessionRegistry&&) = delete;
  SessionRegistry& operator=(SessionRegistry&&) = delete;

  // Every registration represents a new instance; it creates no attachments.
  // Returns kInvalid if the registry's ID space is exhausted.
  [[nodiscard]] PeerId RegisterPeer();
  [[nodiscard]] TargetId RegisterTarget(TargetDescriptor descriptor = {});

  // At most one live attachment per (peer, target). Repeated Attach returns the
  // existing ID. Admission policy belongs to the caller; this checks liveness.
  [[nodiscard]] AttachResult Attach(PeerId peer, TargetId target);
  // The peer argument must come from the caller's trusted connection context.
  [[nodiscard]] CloseResult Detach(PeerId peer, AttachmentId attachment);

  // Trusted lifecycle operations. Removing an unknown/already removed endpoint
  // returns its NotFound status and no effects. Other endpoints remain live.
  [[nodiscard]] CloseResult RemovePeer(PeerId peer);
  [[nodiscard]] CloseResult RemoveTarget(TargetId target);

  [[nodiscard]] bool HasPeer(PeerId peer) const;
  [[nodiscard]] bool HasTarget(TargetId target) const;
  [[nodiscard]] std::optional<Target> FindTarget(TargetId id) const;
  // Live target snapshots in ascending target-ID order. Modifying a returned
  // value cannot change the registered descriptor.
  [[nodiscard]] std::vector<Target> Targets() const;
  [[nodiscard]] std::optional<Attachment> FindAttachment(AttachmentId id) const;
  // Unknown endpoints yield empty snapshots. Results never reference storage.
  [[nodiscard]] std::vector<Attachment> AttachmentsForPeer(PeerId peer) const;
  [[nodiscard]] std::vector<Attachment> AttachmentsForTarget(
      TargetId target) const;

 private:
  std::uint64_t AllocateId();
  CloseResult CloseAttachments(const std::vector<Attachment>& attachments,
                               AttachmentCloseReason reason);

  std::uint64_t next_id_ = 1;
  std::set<PeerId> peers_;
  std::map<TargetId, Target> targets_;
  // A single source of relationship truth. Linear endpoint lookups keep this
  // initial slice small; secondary indexes can be added when justified.
  std::map<AttachmentId, Attachment> attachments_;
};

}  // namespace lynx::debug_router

#endif  // LYNX_DEBUG_ROUTER_SESSION_REGISTRY_H_
