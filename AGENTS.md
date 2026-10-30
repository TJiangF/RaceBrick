# Agent Instructions

- After every code change, build/verify the changed project, then commit and `git push` to `origin main`. Do not wait for the user to ask again.
- Keep `local.properties` and all secrets out of git.
- For Android changes, update the version code/name and produce a debug APK before pushing.
- For firmware changes, build before pushing; flash only when the user asks.
