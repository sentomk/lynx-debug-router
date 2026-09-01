# DebugRouter Roadmap

Status: Android-first native lifecycle and bounded request-correlation foundations
implemented; the wider SDK design remains proposed.

Updated: 2026-09-01.

Current slice: a C++17 `SessionRegistry` with typed, registry-local handles,
immutable target registration metadata, idempotent attach per live peer-target
pair, ownership-checked detach, and scoped endpoint cleanup that returns closed
relationship snapshots. Target metadata currently contains the opaque template
URL supplied by a Lynx view slot; it does not contain platform objects, callbacks,
or a fixed protocol type. A `RequestTracker`
now validates request ownership, maps backend IDs to original frontend IDs and
attachments, and consumes closure results to invalidate pending work. It supports
explicit abandonment and caller-driven expiration of monotonic deadlines. It
rechecks attachment liveness before returning a response route and never recycles
issued IDs within its lifetime. Both modules have deterministic host tests and
Android NDK builds. JNI, transports, CDP parsing/rewriting, subscriptions, timer
scheduling, and backend integration remain unimplemented. See the
[README](../README.md) for the contracts and build instructions. ECS remains a
design mindset, not an imposed module structure.

## Direction and scope

Build a debugging system that lets multiple frontends work with the same App,
join and leave independently, and receive the responses and events intended for
them. Begin with the App SDK, while defining the contracts that the connector,
remote relay, and frontend integration will need to satisfy.

The SDK is the authority for debugging relationships and request correlation.
The connector and remote relay remain responsible for connection-level
addressing and forwarding. They are stateful gateways, not passive byte pipes.

This is a fresh design. Existing multiplexer changes are not a reference or a
starting point. Backward compatibility and migration are deferred; this roadmap
does not promise unchanged wire formats or support for older SDKs.

The initial goal is shared debugging with independent request ownership,
subscriptions, and connection lifecycles. It is not a separate virtual debugger
for every frontend. A command that changes the shared target's execution state
can legitimately affect what other frontends observe.

## Design mindset

Borrow the way ECS encourages reasoning about identity, state, and behavior,
without adopting its entity/component/system decomposition as a required
architecture. Start with the facts the SDK must represent, the relationships
between them, and the rules that must hold when they change.

In particular:

- Distinguish identity, mutable state, relationships, and behavior before
  deciding which responsibilities belong together in code.
- Make important relationships explicit instead of hiding them in object
  pointers, inheritance, or ownership chains.
- Define the state transition and its invariants before choosing containers,
  classes, or module names.
- Keep state changes testable independently of network I/O and backend execution.
- Add relationship lookup and lifecycle rules where the domain needs them;
  do not assume that an ECS abstraction supplies those rules.

There is no requirement for an ECS framework, a generic relationship engine, a
World abstraction, or three modules named Entity, Component, and System. Dense
storage, archetypes, and data-parallel processing are not goals. Even a name such
as `SessionRegistry` is a possible outcome of the design, not the starting point.

## Responsibility boundaries

These are responsibilities to separate, not a fixed class or directory layout.

| Area | Responsibility | Boundary |
|---|---|---|
| SDK relationship and lifecycle logic | Track clients, targets, attachments, subscriptions, and their validity | Does not perform socket I/O or interpret individual CDP methods |
| SDK protocol adaptation | Correlate requests and responses; handle CDP-specific IDs and shared-domain behavior | Does not own physical connections |
| SDK transport adaptation | Translate physical links into logical client events and addressed sends | Does not decide target subscriptions or CDP request ownership |
| App/backend integration | Register target instances, execute operations, and return responses and events | Does not need to understand frontend connection topology |
| Connector | Discover devices and Apps, maintain local links, multiplex frontend channels, and forward to a specified channel | Does not maintain a second CDP request-correlation or attachment authority |
| Remote relay | Manage remote connection membership and addressed forwarding | Exposes equivalent logical-channel behavior without requiring the connector's implementation |
| Frontend integration | Discover targets, attach and detach, send commands, and consume results | Does not allocate globally coordinated CDP IDs |

A physical link can carry multiple logical clients, including a USB link between
the connector and the App. The transport contract must preserve frontend origin,
support an explicit return destination, and report logical-client departure even
when the shared link remains open.

The receiving boundary must bind source identity to the actual connection or
gateway channel. The SDK must validate attachment ownership rather than trust
an arbitrary peer identifier supplied in a request. Exact authentication and
wire encoding remain design work.

## Working vocabulary and invariants

Use the following terms to discuss behavior without committing to a particular
storage model:

| Term | Meaning |
|---|---|
| Transport | A physical communication link into the App SDK |
| Peer | A logical debugging client reachable through a transport |
| Target | A debuggable object, such as a Lynx View, JS runtime, or App-level capability |
| Attachment | One debugging relationship between a peer and a target |

An attachment needs an identity and a lifetime because requests and
subscriptions can outlive the operation that created it. That does not require
representing it as an ECS entity or a particular class. Do not overload one
session identifier to mean a view, an attachment, and a protocol-internal session.

The first behavior model must preserve these invariants:

- Being connected does not automatically grant access to or subscribe a peer to
  every target. Target availability and per-peer subscriptions are separate facts.
- Every live attachment refers to a live peer and target instance. The SDK checks
  who may use or close that attachment.
- A peer may have attachments to multiple targets, and a target may be shared by
  multiple peers. Closing one attachment does not close the others.
- Peer departure ends that peer's attachments, not the targets themselves.
  Target destruction ends its attachments, not the peers themselves.
- Losing a transport affects the peers carried by that transport, not unrelated
  links. Reconnection does not silently revive old attachments.
- Old handles, callbacks, and responses must not bind to replacement connections
  or target instances. Generation or equivalent identity rules must be explicit.
- Every pending request has an identifiable owner. An unrecognized response is
  never broadcast as a fallback.
- State and relationship indexes remain consistent before externally visible
  effects are dispatched. Reentrant callbacks must not observe partial updates.

Prefer one serial execution context for SDK routing state. I/O and backend work
may execute asynchronously, but their results, timeouts, and lifecycle events
must enter that context before changing routing state. This constrains races
within that state; it does not eliminate all concurrency problems in the SDK.

## Delivery sequence

The sequence is dependency-driven, not a delivery-date commitment. The immediate
work is the App SDK. Connector and remote integration follow after the SDK's
behavior and boundary contracts can be exercised without real transports.

### 1. Specify relationships and lifecycle transitions

Begin with the domain behavior, not an ECS container, socket implementation, or
preselected registry class. Describe the stored facts, required lookups,
ownership rules, and observable outcomes of each operation.

| Operation | Required outcome |
|---|---|
| Peer arrives | Record a reachable client without implicitly attaching it |
| Target is registered | Capture host-provided metadata and make that target instance available without creating frontend relationships |
| Attach | Validate both endpoints and access, then return a new relationship handle |
| Detach | Validate ownership and end only the selected relationship |
| Peer leaves | End its attachments and identify their pending work and resources for cleanup |
| Target is removed | End its attachments and identify affected peers for notification |

Specify duplicate operations, invalid endpoints, repeated close operations, and
stale handles. The first implementation reuses an attachment for repeated requests
on the same live peer-target pair; after detach it issues a fresh ID. Derive module
boundaries from these rules rather than prescribing a generic relationship layer.

Completion evidence: a coherent transition specification and test scenarios in
which A and B attach to one target, A leaves without invalidating B, the target
is destroyed, and B's old attachment can no longer be used. The first slice covers
these transitions and documents their contract in the public API and README;
transport identity binding and target access policy remain outside it.

### 2. Implement a deterministic in-memory SDK foundation

Implement the agreed transitions with no dependency on USB, WebSocket, platform
callbacks, or CDP parsing. Make notifications and cleanup actions explicit
outputs so tests can inspect them without performing I/O.

Keep the initial implementation serially callable. Introduce the real executor
at the integration boundary rather than making state storage start threads.

Completion evidence: unit tests cover the lifecycle specification, relationship
consistency, ownership checks, duplicate operations, and stale-instance rejection.
The implementation has no embedded transport or backend side effects.

The initial `SessionRegistry` implements this in-memory scope using ordinary
containers. The owner passes its closure results to `RequestTracker` for request
cleanup. Client notifications remain future work; neither module sends messages
or performs backend side effects.

The first target descriptor stores an opaque `template_url`, matching the value
provided by the real Lynx view message channel when it plugs a DebugRouter slot.
The URL is descriptive rather than identifying: empty and duplicate values are
valid, and every registration still creates a fresh target instance. Snapshot
queries support later target discovery without retaining a View, callback, or
legacy DebugRouter session ID. The bidirectional `(type, payload)` boundary to a
real Lynx target remains a subsequent integration step.

### 3. Add request correlation and event delivery

Bind pending work to attachments. For CDP, allocate distinct backend request IDs,
record the originating attachment and original ID, and restore that ID before
returning a response to its owner. Keep the protocol-specific mapping out of
transport code.

For example, A and B can both send request `id: 1`; the backend receives distinct
IDs, and each frontend receives its own response with `id: 1`, even when responses
arrive in reverse order. Correlation must cover the actual shared backend channel,
which may not correspond one-to-one with a view.

Specify supported ID ranges, exhaustion, and reuse rules from the complete
backend path. Do not assume that an int64 sender interface implies int64 support
throughout CDP processing. Internal request tokens can differ from wire IDs.
Removing a timed-out mapping must not allow a late response to match a new request.

Classify requests, responses, and notifications according to their protocol and
direction; the presence or absence of an `id` field is not a universal message
classification rule. Protocols that already preserve request context need not
use CDP-style rewriting.

Deliver notifications according to live attachment subscriptions, with an
explicit scope for App-level events. Bound pending work and expire abandoned
requests. A timeout or attachment closure ends local waiting; it does not prove
that the backend operation was canceled. Do not automatically retry commands
whose side effects may already have occurred.

Completion evidence: tests cover colliding frontend IDs, out-of-order responses,
backend errors, notification fan-out, timeouts, bounded capacity, late responses,
and peer or target removal while requests are pending.

The current tracker covers the numeric correlation and lifecycle subset of this
step, using a fake backend. Callers explicitly configure the pending capacity and
the positive backend-ID ceiling within int32. IDs are unique across all targets
handled by that tracker and are not reused after any request ending. A tracker
must cover the lifetime of its shared backend response source; replacing it
requires retiring or fencing that source first.

Each request now carries an explicit monotonic deadline. The owner can abandon a
request after a send failure or drive `Expire(now)` to release overdue records.
These operations return cleanup snapshots and reasons without closing attachments
or canceling backend execution. A closed attachment takes precedence over a
timeout/abandonment reason if invalidation was missed. Responses and local endings
consume records at most once in serial processing order; time passing alone does
not end a request. Explicit-time tests cover deadline boundaries and all 24
orderings of response, abandonment, expiration, and detach/invalidation.

Protocol encoding, real-backend range validation, send-failure detection, timeout
scheduling and duration policy, client error delivery, and event subscriptions
still need implementation. A full pending table rejects new requests rather than
growing without limit; its owner must drive expiration to recover overdue capacity.

### 4. Integrate real targets and shared debugging behavior

Connect App and Lynx target registration, command dispatch, and backend callbacks
to the validated SDK model. Marshal state changes onto the serial context without
blocking it on backend execution.

Define which operations change shared target state and how their effects become
visible to other frontends. Independent response routing does not make operations
such as resume or domain disable private to a frontend.

For each supported shared domain, specify initialization, enable/disable behavior,
and cleanup on detach. Where appropriate, aggregate attachment-level demand;
do not assume generic reference counting fully models every domain. A departing
frontend must not blindly disable a capability still needed by another one.

New attachments need an initial state and a consistent transition to subsequent
events. Define snapshot/event ordering and the ownership policy for capabilities
that cannot be shared. Full per-frontend debugger-state virtualization remains
outside the initial goal.

Completion evidence: two simulated frontends exercise the real backend, including
late attachment, shared-state changes, independent departure, and target teardown.

### 5. Add transport adapters and gateway integration

First exercise logical-peer arrival, departure, incoming messages, and directed
sends using a fake transport. Then implement the connector/local and remote paths
against the same behavioral contract.

Define framing, source binding, channel namespaces, link generations, ordering,
failure reporting, and addressed return delivery. The connector must preserve
distinct frontend channels through the shared App link; the SDK must not depend
on a connector being present on the remote path.

Bound per-peer output queues and input sizes. Choose explicit overload behavior
so a slow frontend cannot block other frontends or grow App memory without limit.
Responses and essential state transitions must not be silently dropped. Verify
that local and remote peers can coexist without transport replacement.

Completion evidence: contract tests pass for fake and real adapters; integration
tests cover multiple peers per link, simultaneous links, partial disconnects,
reconnects, stale frames, and slow consumers.

### 6. Harden and document the SDK contract

Consolidate platform integration, resource limits, shutdown ordering, error
reporting, and observability. Record enough correlation and lifecycle metadata to
diagnose a route without requiring full payload logging by default.

Document target registration, peer admission, attachment operations, protocol
extension boundaries, threading expectations, and failure outcomes based on the
implemented behavior. Finalize frontend integration against that contract.

Completion evidence: deterministic lifecycle and protocol tests, transport
contract tests, real-backend end-to-end tests, and resource-pressure tests agree
on the same observable behavior. Supported platforms and capabilities are stated
explicitly rather than inferred from the architecture.

## Decisions to make during the roadmap

- The initial target inventory and how Views, runtimes, and App-level capabilities
  relate to one another.
- Whether later use cases require multiple attachments for one peer-target pair;
  the current slice uses one. Access rules, subscription defaults, and the boundary
  between target availability and frontend interest also need further design.
- Identity scopes and generations across peers, target instances, transport
  channels, and backend request IDs.
- Which debugging capabilities are shared, aggregated, or exclusive, and how a
  new frontend acquires their current state.
- Concrete limits and failure policies for pending requests, message sizes, and
  slow consumers.
- Wire encoding, authentication, and the exact SDK/gateway interfaces after the
  in-memory behavior is established.

These decisions should follow concrete behavior and tests. Backward compatibility
and migration remain a separate, later discussion rather than implicit constraints
on this first design.
