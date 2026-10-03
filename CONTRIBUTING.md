# Contributing

Start with [BUILDING](BUILDING.md), the [editing guide](docs/today-editor-guide.md)
and [ROADMAP](ROADMAP.md). Keep changes focused on a usable, validated workflow.
Do not add a menu item that silently ignores unsupported document content.

Preserve legacy pixel behavior and immutable document ownership. Run expensive
pixel/codec/storage work on workers. Admit temporary and retained allocations
before creating them, retain charges for backing lifetimes, and preserve the
original document on cancellation or failure. An unavailable resource must
produce an explicit failure, not a partially installed edit.

New pixel edits must support a single undoable transaction, no-op redo
preservation, safe stale-job rejection and exact native save/reopen. Test actual
output and failure preservation when adding behavior. Do not infer appearance
compatibility with reference fixtures from CPU/GPU agreement alone.

A saved-format change must update the format specification, SQL schemas,
serializer, reader, version handling and compatibility fixtures together.
Unknown required features must fail explicitly. Keep old Linux schemas readable
without rewriting files simply because they were opened.

Use American spelling and the surrounding C++ style. Compile with the supplied
warnings and preserve checked coordinate/byte arithmetic. Use one build job and
serial tests on low-memory hosts.

For pull requests, describe the user-visible result, material limitations and
checks actually executed. Record the backend for rendering evidence. A small
workflow passing does not establish large-image, low-resource or professional
output qualification. Preserve MIT and third-party attribution when moving or
adapting code. Preparing a repository does not authorize remote publication.
