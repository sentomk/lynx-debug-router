# lynx-debug-router

An Android-first debugging SDK, starting with small C++17 relationship, lifecycle,
and request-correlation implementations. See the [roadmap](docs/roadmap.md) for the
broader direction.

The current deliverable is a native static library, not an Android AAR or a
working debugger. JNI, Java/Kotlin APIs, transports, subscriptions, CDP message
parsing/rewriting, and Lynx integration are not implemented yet.

## SessionRegistry

The [public API](native/include/lynx_debug_router/session_registry.h) manages three
concepts: a `PeerId` identifies a logical debugging client, a `TargetId` identifies
a debuggable target instance, and an `AttachmentId` identifies their relationship.
These are domain concepts, not an ECS framework decomposition.

| Operation | Contract |
|---|---|
| `RegisterPeer` / `RegisterTarget` | Issue a fresh typed handle; create no attachments |
| `Attach` | Require live endpoints; return the existing attachment for a repeated live peer-target pair |
| `Detach` | Require a live peer that owns the attachment; close only that relationship |
| `RemovePeer` | Close its attachments; keep targets and other peers alive |
| `RemoveTarget` | Close its attachments; keep peers and other targets alive |
| `FindAttachment` / `AttachmentsForPeer` / `AttachmentsForTarget` | Return value snapshots, never mutable references into the registry |

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
| `Track` | Validate a live peer and its attachment; allocate a backend ID and remember the original frontend ID |
| `Complete` | Consume one response ID; return its original ID and attachment/peer/target snapshot only if the attachment is still live |
| `Invalidate` | Consume a successful registry closure result and remove requests for its closed attachments |
| `PendingCount` | Return the number of stored requests, including stale records not yet consumed or invalidated |

Create one tracker for each shared backend request-ID space, not one per peer or
per target when those clients share a response channel. Supply explicit
`RequestTrackerLimits`: a positive pending-request capacity and a positive,
inclusive backend-ID ceiling no larger than `INT32_MAX`. The protocol adapter
must choose a ceiling supported by the complete backend path; an int64 API alone
does not establish that range. Invalid limits make `Track` return `kInvalidLimits`.

Backend IDs start at 1 and are never recycled by a tracker, even after completion
or invalidation. Exhaustion returns `kIdExhausted` without altering pending work.
There is no reset API: do not replace a tracker on a still-active backend channel
to reclaim IDs. Retire or fence the old response source before starting a new
tracker, so old replies cannot enter the new ID space.

Original frontend IDs are preserved as int64 values; validating their wire format
belongs to the protocol adapter. The same original ID can be pending on different
attachments. A duplicate on the same attachment is rejected until the previous
request completes or is invalidated. `Track` checks limits, peer existence,
attachment existence, ownership, duplicates, capacity, then ID exhaustion, in
that order. Rejected requests do not consume backend IDs or pending slots.

A typical integration sequence is:

1. Keep the registry alive longer than the tracker, and serialize calls to both.
2. Call `Track` with the peer from trusted connection context and a live attachment.
   Only forward the backend request after successful registration.
3. On a response, call `Complete`. Use the returned original ID and route snapshot
   in the same serial turn, or revalidate before deferred delivery. Success and
   error payloads use the same correlation mechanism; their bodies stay outside
   the tracker. A missing result means discard the response, never broadcast it.
4. After registry detach or endpoint removal, promptly pass its `CloseResult` to
   every affected tracker. `Invalidate` returns removed request snapshots in
   backend-ID order after cleanup, for the caller's own bookkeeping.

Unknown and duplicate responses are harmless. `Complete` also rechecks registry
liveness, so an omitted or delayed invalidation cannot route a response through a
closed attachment. Such a response still releases its pending slot. Repeated or
failed closure results do nothing, and snapshots claiming to close a still-live
attachment are ignored. Returned snapshots do not grant permission to send to an
attachment that closes after the snapshot was returned.

There are no callbacks, automatic retries, timers, deadlines, or backend
cancellation. Nonresponding requests remain pending until completion or attachment
invalidation; the capacity limit bounds their count but does not expire them.
Send-failure handling and timeout policy are later integration work. No JSON
dependency, transport, or ECS infrastructure is introduced by this slice.

## Host build and tests

Requires CMake 3.18+, Ninja, and a C++17 compiler. No downloaded test dependencies
are needed. The test runner uses checks that remain active in Release builds.

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The 29 tests comprise 13 registry cases and 16 tracker cases. They cover lifecycle
invariants, snapshot isolation, duplicate IDs, ownership, bounded capacity, ID
exhaustion, scoped invalidation, and stale responses. A fake backend controls
response order without knowing frontend identities. The end-to-end in-memory
case sends `id: 1` from A and B, delivers B's response first, removes A, discards
A's late response, and verifies that B can continue.

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
