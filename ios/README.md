# RaceBrick iOS

SwiftUI iOS client for the RaceBrick Cloud platform.

- Server: `https://racebrick.tfworks.cn`
- Account: register/login, bind ESP by `device_uid`, view Tracks, Race Logs and legacy Race Logs.
- Race data and fastest-lap processing are server-side; the app never connects to the ESP HTTP API.
- Track maps are drawn with SwiftUI Canvas and can load a 50% satellite overlay from `/api/v1/nav`.
- Track start position/width/offset/direction edits are sent to the cloud; the device receives a queued command on its next sync.
- BLE transport uses the existing RaceBrick service:
  - service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
  - write `6E400002-...`
  - notify `6E400003-...`
- BLE map streaming uses Apple MapKit snapshots, quantized to the existing 320×240 16-color RLE frame format.
- Media page intentionally documents iOS limitations: iOS cannot read third-party app notification lyrics/media metadata. The Android NotificationListener behavior has no public iOS equivalent.

## Build

Requirements: Xcode 15+ with an iOS 16+ SDK. This repo includes a generated
`RaceBrick.xcodeproj`; `project.yml` is the XcodeGen source.

```bash
cd ios
xcodegen generate   # only when project.yml changes
open RaceBrick.xcodeproj
```

Select a development team in Signing & Capabilities, then run on a physical
device. BLE background streaming and App Store distribution require the normal
Apple provisioning setup.

The local Mac used for the initial implementation has only Command Line Tools,
so no iOS SDK build/run was possible here. The shared `RaceBrickCore` package
and its API client/PAL16/BLE codec tests were typechecked and exercised against
the live cloud API from macOS.

## Admin

The cloud admin account is managed by the server. Default reserved account:
`admin` / `RaceBrickAdmin#2026` (change `RACEBRICK_ADMIN_PASSWORD` before first
production initialization if you need a different password).

## Layout

```
ios/
├── project.yml
├── RaceBrick.xcodeproj
├── RaceBrickCore/          # Swift package: models, cloud API, pal16, BLE codec
└── RaceBrick/              # SwiftUI + BLE + MapKit app
```
