# lynx-debug-router

An Android-first debugging SDK, starting with small C++17 relationship, lifecycle,
and request-correlation implementations. See the [roadmap](docs/roadmap.md) for the
broader direction.

The current deliverable is a native static library, not an Android AAR or a
working debugger. JNI, Java/Kotlin APIs, transports, subscriptions, CDP message
parsing/rewriting, real target message delivery, and Lynx integration are not
implemented yet.

## SessionRegistry

The [public API](native/include/lynx_debug_router/session_registry.h) manages three
concepts: a `PeerId` identifies a logical debugging client, a `TargetId` identifies
a debuggable target instance, and an `AttachmentId` identifies their relationship.
A `Target` snapshot combines its ID with registration metadata. These are domain
concepts, not an ECS framework decomposition.

| Operation | Contract |
|---|---|
| `RegisterPeer` | Issue a fresh peer handle; create no attachments |
| `RegisterTarget` | Capture a descriptor for a new target instance, issue a fresh target handle, and create no attachments |
| `Attach` | Require live endpoints; return the existing attachment for a repeated live peer-target pair |
| `Detach` | Require a live peer that owns the attachment; close only that relationship |
| `RemovePeer` | Close its attachments; keep targets and other peers alive |
| `RemoveTarget` | Close its attachments; keep peers and other targets alive |
| `FindTarget` / `Targets` | Return copies of live target metadata, ordered by target ID for the collection query |
| `FindAttachment` / `AttachmentsForPeer` / `AttachmentsForTarget` | Return attachment value snapshots, never mutable references into the registry |

`TargetDescriptor` currently contains one opaque `template_url`, based on the URL
provided when a Lynx view plugs its DebugRouter slot. The registry stores a copy
without parsing, normalizing, or validating it. Empty strings, malformed URLs,
and duplicate values are allowed; validation and input-size policy belong to the
host integration. A URL describes a target but is not its identity. Registering
two live views with the same URL creates two target IDs, and registering the same
URL after target removal creates another fresh ID.

There is no target metadata update operation in this slice. `FindTarget` and
`Targets` return independent copies, so modifying a snapshot cannot alter the
registered descriptor. Removing a target removes its descriptor together with
the target identity and its attachments. Snapshots obtained before removal remain
ordinary historical values and do not keep the target alive.

The registry does not store an Android `View`, Lynx object, callback, message
channel, or fixed protocol type. Real Lynx messages carry their own `(type,
payload)` pair; defining that bidirectional target boundary is later work. The
legacy DebugRouter `session_id` is not stored or treated as an attachment ID.

Detach and removal return `CloseResult`: a status and the closed attachment
snapshots with their closure reasons. Results are ordered by attachment ID. All
state changes are complete before the caller receives the result; the registry
does not invoke callbacks, send notifications, or cancel backend work.

Unknown or previously removed handles return the corresponding `NotFound` status
without effects. Detaching another live peer's attachment returns `kNotOwner`.
Attach validates the peer before the target; detach validates the peer, then the
attachment, then ownership. Unknown endpoint queries return empty snapshots.

Handles are never reused within one registry's lifetime. Detaching and attaching
the same pair again creates a new attachment ID. Re-registering a departed peer
or destroyed target creates a new endpoint ID; stale operations cannot affect
those replacements. On ID exhaustion, registration returns `kInvalid` and a new
attach returns `kIdExhausted`; existing relationships remain usable.

Handles are local to their issuing registry, not globally unique or wire IDs.
Never pass them between registry instances. The owner must drain queued work
before destroying the registry and serialize **all** calls, including reads.
The registry owns no executor, locks, sockets, or platform objects and cannot be
copied or moved. Admission policy and binding an incoming request to its trusted
peer are caller responsibilities; numeric handles are not authentication tokens.

The initial storage uses standard-library containers and scans the attachment
table for endpoint lookups. There are no secondary indexes or generic relationship
engine. This keeps the state model small while its lifecycle contract is tested.

## RequestTracker

The [request API](native/include/lynx_debug_router/request_tracker.h) binds pending
numeric requests to attachments in one `SessionRegistry`. It returns routing
metadata, not serialized messages, and leaves the registry unchanged.

| Operation | Contract |
|---|---|
| `Track` | Validate a live peer and its attachment; allocate a backend ID and remember the original frontend ID and explicit deadline |
| `Complete` | Consume one response ID; return its original ID and attachment/peer/target snapshot only if the attachment is still live |
| `Abandon` | End local waiting for one backend ID, for example after a send failure; return cleanup metadata and a reason |
| `Expire` | End requests whose deadline is at or before the supplied time; return cleanup metadata in backend-ID order |
| `Invalidate` | Consume a successful registry closure result and remove requests for its closed attachments |
| `PendingCount` | Return the number of stored requests, including stale or overdue records not yet consumed |

Create one tracker for each shared backend request-ID space, not one per peer or
per target when those clients share a response channel. Supply explicit
`RequestTrackerLimits`: a positive pending-request capacity and a positive,
inclusive backend-ID ceiling no larger than `INT32_MAX`. The protocol adapter
must choose a ceiling supported by the complete backend path; an int64 API alone
does not establish that range. Invalid limits make `Track` return `kInvalidLimits`.

Backend IDs start at 1 and are never recycled by a tracker, even after completion,
abandonment, expiration, or invalidation. Exhaustion returns `kIdExhausted` without
altering pending work. There is no reset API: do not replace a tracker on a
still-active backend channel to reclaim IDs. Retire or fence the old response
source before starting a new tracker, so old replies cannot enter the new ID space.

Original frontend IDs are preserved as int64 values; validating their wire format
belongs to the protocol adapter. The same original ID can be pending on different
attachments. A duplicate on the same attachment is rejected until the previous
request ends. `Track` checks limits, peer existence, attachment existence,
ownership, duplicates, capacity, then ID exhaustion, in that order. Rejected
requests do not consume backend IDs or pending slots.

A typical integration sequence is:

1. Keep the registry alive longer than the tracker, and serialize calls to both.
2. Call `Track(peer, attachment, original_id, deadline)` with the peer from trusted
   connection context, a live attachment, and an absolute monotonic deadline.
   Only forward the backend request after successful registration. If sending
   fails, call `Abandon` with the issued backend ID to release its pending slot.
3. On a response, call `Complete`. Use the returned original ID and route snapshot
   in the same serial turn, or revalidate before deferred delivery. Success and
   error payloads use the same correlation mechanism; their bodies stay outside
   the tracker. A missing result means discard the response, never broadcast it.
4. After registry detach or endpoint removal, promptly pass its `CloseResult` to
   every affected tracker. `Invalidate` returns removed request snapshots in
   backend-ID order after cleanup, for the caller's own bookkeeping.
5. Drive `Expire(now)` from the same serial context to reclaim overdue requests.
   Consume its results only after it returns; all selected records are already
   removed at that point.

Unknown and duplicate responses are harmless. `Complete` also rechecks registry
liveness, so an omitted or delayed invalidation cannot route a response through a
closed attachment. Such a response still releases its pending slot. Repeated or
failed closure results do nothing, and snapshots claiming to close a still-live
attachment are ignored. Returned snapshots do not grant permission to send to an
attachment that closes after the snapshot was returned.

### Deadlines and local termination

`RequestTime` is `std::chrono::steady_clock::time_point`. The caller chooses each
deadline and supplies `now` from the same monotonic clock. Tests can construct
time points directly without reading a clock or sleeping. Every `Track` call must
provide a deadline; there is no default. `Track` stores the supplied deadline without
reading time; even a past deadline is accepted and is eligible for the next
`Expire` call. `deadline == now` expires, and `RequestTime::max()` is an ordinary
deadline, not a sentinel. The caller is responsible for safely computing deadlines
within the clock's representable range.

Time passing alone does not mutate the tracker. `Complete` and `Abandon` do not
check the clock. The first processed ending consumes the request; later responses
or cleanup calls cannot end it again. To give deadlines precedence over queued
responses, call `Expire(now)` before processing those responses. Also drive
expiration before admission if overdue requests should no longer occupy capacity.
Scheduling and timeout duration policy belong to the owner, not this module.

`Abandon` returns an optional `EndedRequest`; `Expire` returns a vector of them.
Each contains the original `PendingRequest` snapshot, including its deadline,
and a `RequestEndReason`: `kAbandoned` or `kTimedOut`. If the registry already
closed the attachment, either path instead reports `kAttachmentClosed`, even if
the owner missed `Invalidate`. Cleanup still releases capacity in that case.
These are bookkeeping records, not sendable routes. Do not deliver a result for
`kAttachmentClosed`; recheck attachment liveness before notifying a client for
any other reason. A later closure can invalidate a previously returned snapshot.

`Abandon` is a trusted owner operation using a backend ID, not a frontend
cancellation API. Neither it nor expiration closes attachments, cancels backend
execution, or retries a command whose effects may already have occurred. The
frontend can reuse its original request ID after local waiting ends, but the new
request gets a fresh backend ID so a late old response cannot match it.

There are no callbacks, automatic retries, timer threads, or backend cancellation.
Actual send-failure detection, client error encoding, and notification delivery
remain integration work. No JSON dependency, transport, or ECS infrastructure is
introduced by this slice.

## Host build and tests

Requires CMake 3.18+, Ninja, and a C++17 compiler. No downloaded test dependencies
are needed. The test runner uses checks that remain active in Release builds.

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The 40 tests comprise 14 registry cases and 26 tracker cases. They cover lifecycle
invariants, snapshot isolation, duplicate IDs, ownership, bounded capacity, ID
exhaustion, target metadata snapshots, duplicate target URLs, scoped invalidation,
and stale responses. A fake backend controls response order without knowing
frontend identities. The end-to-end in-memory case sends `id: 1` from A and B,
delivers B's response first, removes A, discards A's late response, and verifies
that B can continue.

Explicit-time tests cover inclusive deadlines, equal and out-of-order deadlines,
time-point extremes, abandonment, capacity recovery, missed invalidation, and
frontend ID reuse after timeout without cross-delivering a late response. All 24
orderings of response, abandonment, expiration, and detach/invalidation are tested
to ensure exactly one ending. No timeout test depends on wall-clock time or sleep.

For AddressSanitizer and UndefinedBehaviorSanitizer on a supported host compiler:

```sh
cmake -S . -B build/host-sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build/host-sanitized
ctest --test-dir build/host-sanitized --output-on-failure
```

The native suites pass Debug, Release with exceptions disabled, and a
UBSan-only build (`-fsanitize=undefined -fno-sanitize-recover=all`). ASan validation
is outstanding: during the initial registry validation, both the suite and an
unrelated minimal ASan program timed out on the development host. That limitation
has not been revalidated for this slice and is not counted as a passing check.

## Android NDK build

Set `DEBUG_ROUTER_NDK` to your installed NDK directory. The initial build is
validated with NDK 27.1.12297006 for `arm64-v8a` and `armeabi-v7a`, targeting API 21.

```sh
cmake -S . -B build/android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEBUG_ROUTER_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-21 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build build/android-arm64
```

The output is `build/android-arm64/liblynx_debug_router.a`. For a 32-bit build,
use a separate build directory and `-DANDROID_ABI=armeabi-v7a`. An Android native
consumer can instead use `add_subdirectory` and link `lynx_debug_router::core`;
the target supplies its public include directory and C++17 requirement. The
consumer chooses the C++ runtime for the final Android library or executable.

To build the native test executables for a device, configure the Android build
with `-DBUILD_TESTING=ON`. Copy `session_registry_test` and `request_tracker_test`
to an isolated directory under `/data/local/tmp` using adb and execute them there
on a matching-ABI device. With no arguments each executable runs its entire suite;
a case name runs just that case. Host CTest does not automatically execute a
cross-compiled Android binary.

The native suites also pass on an arm64 Android emulator. This checks lifecycle
and in-memory request correlation, not Java/JNI or end-to-end debugging.
