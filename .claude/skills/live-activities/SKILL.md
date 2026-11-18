---
name: live-activities
description: What iOS really does with Live Activities (ActivityKit) - background update limits, stale dates, ending them, buttons with App Intents, and how to debug them. Use when building or debugging a Live Activity or Dynamic Island UI, or an interactive widget button.
---

# Live Activities

## Updates from the background get dropped

iOS rations Live Activity updates from an app in the background and drops the rest **without an error**:
`activity.update(...)` returns normally. So:

- Don't use a stale date as a heartbeat ("renew it every minute; if the app dies it goes stale"). iOS also marks
  it stale while the app is running fine, when the renewals got dropped, and later updates don't bring it back.
  The activity then shows "stopped" until the app comes to the foreground. Use `staleDate: nil`.
- Make sure the activity can't outlive the app instead:
  - On `UIApplication.willTerminateNotification` (swiped away in the app switcher), end every activity with
    `dismissalPolicy: .immediate`, and wait for it (up to about 2 s, a semaphore), or it stays on the Lock Screen.
  - At launch, end any `Activity<…>.activities` left over from a crash.
- Update only when what it shows changes: compare with the last state sent.

## Starting

- Only from the foreground. If the start fails, try again with the next change while the app is open.
- Check `ActivityAuthorizationInfo().areActivitiesEnabled` first; the user can turn them off per app.
- Keep the content state small: only what's drawn.

## Buttons

- A `Button(intent:)` in the activity runs an App Intent. A `LiveActivityIntent` runs in the app's process, so it
  can reach the app's state through a static handler. Make the handler `async` and await the work, so
  `perform()` returns after it's done.
- To turn the microphone on from the background (an unmute button), the intent must also conform to
  `AudioRecordingIntent` (iOS 18+), and the app needs the `audio` background mode.
- iOS only runs an intent that the bundle's `Metadata.appintents` describes. Xcode generates it at build time;
  other build tools don't (see `ios-builds-without-xcode`).

## Debugging

- Log each step with `os.Logger` under its own category: started, changed, ended, and app lifecycle
  (background, foreground, memory warning). Watch `activity.activityStateUpdates` and log what iOS reports.
- Read the log with the iPhone plugged in: `idevicesyslog -p <process name>`. In a container, write it to a file
  inside the container and copy it out; a long-running pipe through `docker exec` can stall.
- To reproduce background problems: start the activity, lock the phone, and wait a few minutes.
