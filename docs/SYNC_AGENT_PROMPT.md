> **Superseded 2026-11-26.** The active design is in
> [`THREE_TIER_ARCHITECTURE.md`](THREE_TIER_ARCHITECTURE.md) and
> [`API_V2.md`](API_V2.md). The new specification allows automatic cloud
> backup (but never automatic device sync) and limits app edits to Track
> Drafts. This file is kept for historical context only.

# RaceBrick Sync Agent Prompt

Copy the prompt below into the coding agent. Edit this file directly when the
sync requirements change.

```text
# Task: Redesign RaceBrick sync with local-device as primary and cloud as
# low-frequency backup.

## Current reality
- RaceBrick device <-> Android app over local HTTP.
- Android app <-> cloud over HTTPS.
- Device direct cloud sync exists but is intentionally disabled. Keep it disabled.
- Current app cloud sync is not a real three-way sync: it uploads a full device
  snapshot with replace=true and only applies limited track fields from cloud.
- Current local cache is the last cloud snapshot, not an authoritative local
  working copy.
- Current local edits are written directly to the device and then a global
  dirty flag is set.
- Device tracks/sessions/logs have no stable IDs, revision, updated_at, or
  tombstones. Track/session slots and log indexes are reused.
- Two datasets are called "Race Log": raw RaceSession and legacy histrec.
  Define the user-facing Race Log explicitly; do not mix them.

## Product priority
Primary workflow:
  Local App working copy <-> RaceBrick device.

Secondary workflow:
  Local App working copy <-> Cloud backup, manual or low-frequency.

Cloud must never be required for normal local-device operation.
Cloud must not auto-overwrite local data.

## Required data model
Before adding destructive sync UI, introduce:
- stable UUID for Track, Session, Race Log, Lap and satellite assets;
- parent_id relationships;
- per-entity revision;
- content hash (SHA-256);
- created_at and updated_at UTC, nullable when device time is unknown;
- deleted tombstone;
- dataset revision;
- origin/source;
- separate baselines:
  - baselineDevice
  - baselineCloud
- separate dirty flags:
  - dirtyDevice
  - dirtyCloud

Local working data must live in a local database (Room/SQLite) with large
CSV/trace/image blobs in files, not in the existing single cloud cache.

## UX
Replace the current cloud page with a "Sync" page showing:

RaceBrick  --  Local App  --  Cloud

For each node show:
- last modified timestamp
- last successful sync timestamp
- data version
- clean/dirty/conflict/unknown
- counts for tracks, Race Logs, sessions

Show a granular diff relative to baselines:
- Track added/removed/changed:
  name, start_f, start_width, start_offset, direction, geometry/hash
- Race Log/Session added/removed/changed by stable ID
- Do not show a confident diff if stable IDs/hashes are unavailable.

Primary actions:
- Device -> Local (load device data)
- Local -> Device (push local changes)

Secondary cloud actions:
- Local -> Cloud (backup)
- Cloud -> Local (restore manually)

Every destructive action must:
- show affected counts and direction;
- warn about conflicts;
- create a backup;
- stage/validate writes;
- show progress and per-item failures;
- never silently overwrite both-changed data.

## Automatic prompts
Device prompt:
- On RACE entry or device discovery, fetch a lightweight device manifest.
- If device is newer than baselineDevice and local dirtyDevice is false, prompt
  "Load device data?"
- If dirtyDevice is true and device also changed, show conflict; do not auto load.

Cloud prompt:
- Do not check cloud on every RACE entry.
- Use a lightweight background state check at most once per foreground/day.
- Prompt upload only if ALL are true:
  * logged in
  * device data is clean relative to baselineDevice
  * local differs from baselineCloud (localCloudDirty)
  * last successful cloud upload was >= 3 days ago
  * no sync operation is running
  * cloud reachable
  * cloud is not newer than baselineCloud
  * no active race recording
- Prompt is non-blocking: "Local race data has not been backed up for 3 days.
  Upload now?"
- Never auto-upload.
- If cloud is newer than local, do not prompt upload; show "Cloud has newer
  data" and require manual restore.
- Snooze dismissal for at least 24 hours.
- If dirtyDevice is true, prioritize Local -> Device and do not prompt cloud.

## Local-first editing
- Track edits, deletes, associations, lap changes and Race Log changes must
  modify the local database first.
- Do not write the device immediately.
- Yellow frame means local dirtyDevice exists.
- Add "同步修改到 RaceBrick" under the existing satellite upload button.
- The batch push:
  * computes local deltas against baselineDevice;
  * only sends changed entities;
  * is transactional per item where possible;
  * updates baselineDevice only after each successful item;
  * leaves retryable failures visible;
  * never updates cloud automatically.
- Cloud backup remains a separate explicit action.
- Show cloud backup pending as a separate badge, not as the primary yellow frame.

## Device APIs
Add versioned manifest and import/export support:
- GET /api/sync/manifest
- GET /api/sync/entity?id&type
- POST /api/sync/import (idempotent, per-item result)
- Track create/update/import
- Session import including raw samples/CSV
- Race Log import
- optional satellite image manifest/download/upload

Requirements:
- atomic or staged snapshot, not multiple independent GETs;
- block sync while a race is being recorded;
- stable record IDs independent of flash slots;
- tombstones;
- checksums;
- request idempotency key;
- no full replacement unless explicitly selected by the user.

## Cloud APIs
Add:
- lightweight state/manifest;
- incremental changes for all entity types;
- deleted tombstones;
- base_revision/If-Match with 409 conflict;
- idempotency key;
- backup/history restore;
- streaming/compressed session uploads.

Do not use blind replace=true from a phone cache.
Do not allow stale local cache to overwrite newer cloud data.

## Efficiency
- Check only manifest/state during prompts.
- Fetch full payloads only when the user asks to load or sync.
- Only transfer changed entities.
- Stream/compress large CSV and traces.
- Cache hashes and revisions.
- Keep satellite images out of the Race data cloud snapshot; sync them separately.

## Stability and security
- Use one SyncCoordinator for all local/device/cloud operations.
- Serialize operations; no concurrent sync from MainActivity and RaceActivity.
- Persist operation state and idempotency keys.
- Add shared-secret/token auth to destructive device sync APIs.
- Encrypt cloud tokens at rest.
- Validate HTTP status and JSON ok fields.
- Add power-loss/crash/retry tests.
- Back up local and cloud data before overwriting.
- Keep device direct cloud sync disabled.

## Implementation order
1. Add stable IDs, revisions, hashes, timestamps, tombstones.
2. Add local database and baseline/dirty model.
3. Add atomic device manifest and import APIs.
4. Add local-device diff and batch push with yellow frame.
5. Add Sync UI with three-node status/diff.
6. Add manual cloud backup/restore with version checks.
7. Add the 3-day cloud backup prompt and snooze.
8. Add satellite asset sync separately.

## Acceptance tests
- Device changed while local clean -> prompt device load.
- Local dirty and device unchanged -> yellow frame and batch push only.
- Local dirty and device changed -> conflict, no auto overwrite.
- Cloud unchanged for 3 days + local dirtyCloud + device clean -> prompt upload cloud.
- Cloud newer + local clean -> no upload prompt.
- Cloud newer + local dirty -> conflict; no automatic download/overwrite.
- Interrupted push, reboot and retry must not duplicate or lose records.
- Cloud 409 must preserve both versions and show a diff.
- Large session/CSV upload must not be Base64-bloated or block UI.
- Satellite upload failure must not mark Race data sync as failed.
```
