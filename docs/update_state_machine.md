# The update state machine

How one call to `ota_api_update()` gets from an idle component to a new boot partition: which
function drives each step, which of them can refuse to go on, and what is left behind when one
fails. The code is in [src/ota-api.c](../src/ota-api.c), with the run state in
[src/ota-api-state.c](../src/ota-api-state.c) and the reporting in
[src/ota-api-report.c](../src/ota-api-report.c).

This is the download only. The trial run that follows the reboot an update causes is a separate
machine with its own state and its own lifetime, in [src/ota-api-trial.c](../src/ota-api-trial.c).

## Thread context

Everything below runs on one task — the caller's under `ota_api_update()`, the spawned
`ota_api_task` under `ota_api_start_task()`. There is no network thread. Both `event_cb` and the
`esp_http_client` event handler are called synchronously from inside that task, so blocking in
either one stalls the download and delays `ota_api_abort()`.

## Diagram

```
IDLE                             s_running == false
 │
 │ ota_api_update(config)
 ├──► ESP_ERR_INVALID_ARG        config or config->url is NULL: no slot taken, no
 │                               event dispatched, the caller gets the error back
 ▼
CLAIMING                         ota_api_claim_update_slot()
 ├──► ESP_ERR_INVALID_STATE      another update holds the slot; still no event, and
 │                               nothing of that update is disturbed
 │
 │ s_running = true, s_abort_requested = false
 ▼
CONFIGURING                      run_update() → ota_api_build_http_config()
 ├──► RELEASING                  ESP_ERR_INVALID_ARG; nothing to unwind
 ▼
CONNECTING                       esp_https_ota_begin()
 │                               DNS → TCP → TLS → GET → response headers →
 │                               esp_ota_begin() on the next OTA partition
 ├──► RELEASING                  esp-tls, HTTP or partition error; no handle exists
 ▼
NOTIFYING                        ota_api_dispatch_event(OTA_API_EVENT_STARTED, NULL)
 ├──► ABORTING                   event_cb returned anything but ESP_OK
 ▼
INSPECTING                       check_incoming_image()
 │                                 esp_https_ota_get_img_desc(handle, &new_app)
 │                                 dispatch(OTA_API_EVENT_IMAGE_DESC, &new_app)
 │                               the cheapest refusal point: nothing is in flash yet
 ├──► ABORTING                   the descriptor could not be read, or event_cb
 │                               refused the image
 ▼
DOWNLOADING                      download_image()
 │                               esp_https_ota_get_image_size() once, then per chunk:
 │                                 1 esp_https_ota_perform()    one chunk into flash
 │                                 2 ota_api_abort_requested()  cancellation point
 │                                 3 ota_api_report_progress(force = false)
 │                               repeats while perform() answers
 │                               ESP_ERR_HTTPS_OTA_IN_PROGRESS
 ├──► ABORTING                   ESP_ERR_NOT_FINISHED (abort requested), a transport
 │                               or flash error, or event_cb's verdict
 ▼ perform() returned ESP_OK: the server has nothing more to send
VERIFYING                        still inside download_image()
 │                                 esp_https_ota_is_complete_data_received()
 │                                 ota_api_report_progress(force = true)
 │                               the forced report lands the 100% line and is the
 │                               last chance to refuse
 ├──► ABORTING                   ESP_ERR_OTA_VALIDATE_FAILED on a truncated
 │                               response, or event_cb's verdict
 ▼
COMMITTING                       esp_https_ota_finish()
 │                               validates the written image, then rewrites otadata
 ├──► RELEASING                  ESP_ERR_OTA_VALIDATE_FAILED or another IDF error;
 │                               otadata is left alone
 ▼ ESP_OK — otadata now points at the new partition
RELEASING
 │                               ota_api_release_update_slot()
 │                               s_running = false, s_abort_requested = false
 │                               ota_api_is_running() answers false from here on,
 │                               including while event_cb handles the event below
 ├── err == ESP_OK ──► (void) dispatch(OTA_API_EVENT_SUCCEEDED, NULL)
 └── err != ESP_OK ──► (void) dispatch(OTA_API_EVENT_FAILED, &err)
 │                               both verdicts are discarded: the outcome is settled
 ▼                               and there is nothing left to stop
IDLE                             ota_api_update() returns err; in ota_api_task,
                                 ESP_OK → esp_restart(), otherwise vTaskDelete(NULL)


ABORTING                         entered from the edges above
 │                               esp_https_ota_abort(handle) discards what was
 │                               written, closes the connection and frees the
 │                               handle; otadata is never touched
 ▼
RELEASING
```

## What each state is holding

| State | Driven by | OTA handle | In the target partition | Failure unwinds through |
| --- | --- | --- | --- | --- |
| CLAIMING | `ota_api_claim_update_slot()` | — | — | direct return, no event |
| CONFIGURING | `ota_api_build_http_config()` | — | — | RELEASING |
| CONNECTING | `esp_https_ota_begin()` | created here | — | RELEASING |
| NOTIFYING | `ota_api_dispatch_event()` | held | — | ABORTING |
| INSPECTING | `check_incoming_image()` | held | — | ABORTING |
| DOWNLOADING | `download_image()` | held | a partial image | ABORTING |
| VERIFYING | `download_image()` | held | the whole image | ABORTING |
| COMMITTING | `esp_https_ota_finish()` | released by `finish()` | the whole image | RELEASING |

## The one irreversible transition

Only a successful `esp_https_ota_finish()` at [ota-api.c:155](../src/ota-api.c#L155) makes the new
image bootable, and it is the only line in the component that rewrites `otadata`. Every other exit
— a verdict from `event_cb`, an abort, a dropped connection, a failed validation — reaches
RELEASING with the boot partition still pointing at the running firmware. Bytes may have been
written into the target partition, but a partition nothing boots from is not a state the device can
be stranded in.

That is what makes the three refusal points usable rather than theoretical, in increasing order of
what they throw away:

1. **`OTA_API_EVENT_IMAGE_DESC`** — only the image header has been fetched. Comparing the offered
   version against `esp_app_get_description()` here costs nothing.
2. **`OTA_API_EVENT_PROGRESS` during the loop** — discards the bytes downloaded so far.
3. **`OTA_API_EVENT_PROGRESS` forced in VERIFYING** — the whole image is on flash but
   `esp_https_ota_finish()` has not run, so refusing still discards it cleanly.

## Ordering inside the download loop

The three calls in DOWNLOADING run in a fixed order, and the order is the contract:

- `esp_https_ota_perform()` first, because there is no way to cancel half a chunk. The latency of
  `ota_api_abort()` is therefore one chunk plus however long `event_cb` takes.
- `ota_api_abort_requested()` before the progress report, so a condemned update does not spend a
  callback announcing progress it is about to throw away.
- `ota_api_report_progress()` last, and its return value is control flow, not decoration: anything
  but `ESP_OK` leaves the loop and becomes the update's result.

A report suppressed by `progress_interval_ms` never calls `event_cb` at all, so a throttled tick
cannot stop the update — only a report that actually reached the application can.

`esp_https_ota_is_complete_data_received()` in VERIFYING is checked explicitly because the loop can
end with `ESP_OK` on a response the server truncated. Without it, a partial image would reach
`esp_https_ota_finish()`.

## Where each return value comes from

| Return | Origin | State |
| --- | --- | --- |
| `ESP_ERR_INVALID_ARG` | [ota-api.c:170](../src/ota-api.c#L170) — `config` or `url` is NULL | before CLAIMING |
| `ESP_ERR_INVALID_STATE` | [ota-api.c:175](../src/ota-api.c#L175) — the slot is taken | CLAIMING |
| `ESP_ERR_INVALID_ARG` | [ota-api-http.c:146](../src/ota-api-http.c#L146) — an `http://` URL without `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`; [:170](../src/ota-api-http.c#L170) — the server can be neither validated nor explicitly skipped | CONFIGURING |
| esp-tls / HTTP errors | `esp_https_ota_begin()` | CONNECTING |
| whatever `event_cb` returned | any dispatch before the outcome is settled | NOTIFYING, INSPECTING, DOWNLOADING, VERIFYING |
| `ESP_ERR_NOT_FINISHED` | [ota-api.c:81](../src/ota-api.c#L81) — `ota_api_abort()` was called | DOWNLOADING |
| `ESP_ERR_OTA_VALIDATE_FAILED` | [ota-api.c:99](../src/ota-api.c#L99) — the response was truncated | VERIFYING |
| `ESP_ERR_OTA_VALIDATE_FAILED` | `esp_https_ota_finish()` — the image did not validate | COMMITTING |
| `ESP_OK` | `esp_https_ota_finish()` | COMMITTING |

`ESP_ERR_NO_MEM` belongs to `ota_api_start_task()` alone ([ota-api.c:230](../src/ota-api.c#L230)
and [:239](../src/ota-api.c#L239)): the task or its copy of the config could not be allocated, and
this machine never starts.

## The slot itself

`s_running` and `s_abort_requested` live in [ota-api-state.c](../src/ota-api-state.c) behind a
`portMUX_TYPE` spinlock, and every function that touches them is in that file. The critical
sections are test-and-set operations on a `bool` — none of them blocks, which is why a spinlock is
the right primitive here and why `ota_api_abort()` is safe to call from any task and any core.

`ota_api_claim_update_slot()` clears `s_abort_requested` as it takes the slot, so an abort that
arrived too late to affect the previous update cannot leak into the next one.
