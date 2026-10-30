# Business partner recipes

These recipes were exercised on Bigfox A4H/001. Discover and guard the actual controls on other systems; toolbar positions and labels are not universal. BP may reopen the previously selected partner instead of an empty start screen.

## Open and inspect

Start BP only when needed. Use Open business partner, fill the partner number in the popup, confirm, then verify the requested partner and current display/change mode. On the tested system the popup field is `BUS_JOEL_MAIN-OPEN_NUMBER`. Batch the known popup sequence and return the resulting observation. Do not rely on a title alone to establish the selected partner.

## Change email

Open the requested partner and read the current email if needed. Switch to Change only when necessary. Read editable refs or find the exact email field; the tested address control includes `SZA1_D0100-SMTP_ADDR`, but there can be multiple E-mail controls. Fill the requested value, save under the user's authorization, then reopen and read it back. A successful fill alone is not persistence. For an authorized test, retain the original value privately, restore it, and reopen to verify restoration.

## Create organization

Observe the BP view and enter Create Organization. Read the actual editable form, then fill the requested fields together. Validate mandatory fields from the real screen and supplied task data. Save only when creation is authorized. Capture the assigned partner ID from the Save result immediately, retain it for cleanup, and reopen that exact partner to verify persistence. If the response is lost, investigate the possible created record before another Create.

## Discard an unsaved test

Cancel and inspect the active popup. Confirm discard only when its title and controls identify the expected discard dialog. On the tested system the title is `Cancel Editing of Business Partner`. Stop on an unexpected popup. Verify return to BP or SAP Easy Access without saving.

## Latency evidence

Measure connected call wall time separately from agent-turn time. Exclude process startup and initial COM attachment from steady-state comparisons. Record calls, bytes, successful persistence/discard, failures and available phase timings. Model/orchestration residual is not pure model inference time.
