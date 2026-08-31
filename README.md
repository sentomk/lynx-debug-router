# lynx-debug-router

An Android-first debugging SDK, starting with a small C++17 relationship and
lifecycle implementation. See the [roadmap](docs/roadmap.md) for the broader
direction.

The current deliverable is a native static library, not an Android AAR or a
working debugger. JNI, Java/Kotlin APIs, transports, subscriptions, CDP request
correlation, and Lynx integration are not implemented yet.

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

## Host build and tests

Requires CMake 3.18+, Ninja, and a C++17 compiler. No downloaded test dependencies
are needed. The test runner uses checks that remain active in Release builds.

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The 13 tests cover registration, invalid endpoints, duplicate attach, ownership,
scoped cleanup, stale handles, snapshot isolation, the two-peer shared-target
lifecycle, and relationship consistency through a sequence of removals.

For AddressSanitizer and UndefinedBehaviorSanitizer on a supported host compiler:

```sh
cmake -S . -B build/host-sanitized -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build/host-sanitized
ctest --test-dir build/host-sanitized --output-on-failure
```

The initial validation passed Debug, Release with exceptions disabled, and a
UBSan-only build (`-fsanitize=undefined -fno-sanitize-recover=all`). ASan validation
is outstanding: on the development host, both the suite and an unrelated minimal
ASan program timed out. That run is not counted as a passing memory-safety check.

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

To build the native test executable for a device, configure the Android build
with `-DBUILD_TESTING=ON`. Copy `session_registry_test` to an isolated directory
under `/data/local/tmp` using adb and execute it there on a matching-ABI device.
With no arguments it runs every test; a case name runs just that test. Host CTest
does not automatically execute a cross-compiled Android binary.

The full native test suite also passes on an arm64 Android emulator. This checks
the native lifecycle implementation, not Java/JNI or end-to-end debugging.
