# Search helps and table reads

## Choose a bounded observation

For a known dictionary table, read its current tab with `no_tabs=true`, or select just the Fields tab with `tab=tabpDEF` after observing that tab ID. Do not combine `tab` and `no_tabs`. Expanding every tab executes additional SAP navigation; `only=tables` alone does not prevent it.

`text_contains` filters the fetched page, not the entire SAP table. `returned` is the number of matching rows; `rows_fetched` is the number examined. `next_offset` advances past examined rows, including nonmatches. A page with no matches does not prove absence from later pages. Use SAP selection criteria for a whole-result search, or paginate without a text filter when you need the cursor even on an empty match page.

## F4 repository table search

Observed on Bigfox A4H/001, English SE11. Re-observe IDs and titles on another session or SAP variant. This example searches metadata; it does not create or modify a dictionary object.

1. On **ABAP Dictionary: Initial Screen**, find the unique table field using `id_contains=RSRD1-TBMA_VAL`, `type=GuiCTextField`.
2. Batch the known steps with `stop_on_error=true`:
   - Open `gui_element_f4` on the observed field.
   - Find `id_contains=KEY1-LOW`, `type=GuiTextField`, with `expect` title **Repository Info System: Find Tables** and transaction **SE11**; bind the unique match.
   - Fill the bound criterion, for example `TSTC*`.
   - Send `f8` explicitly to the observed popup window, e.g. `/app/con[0]/ses[0]/wnd[1]`.
   - Read `only=tables`, `no_tabs=true`, `skip_trees=true`, `max_rows=5`, including this final result in summary mode.
3. Inspect the results before choosing a row. Cancel a read-only demo with `gui_popup_close` and `vkey=12`.

`gui_element_fill.then_key` targets the main window; use a separate window-targeted key call inside the batch for F4 dialogs. If the result limit also needs changing, observe its control and fill both requested criteria together. Use `bind`, `expect`, and summary options only when the exposed catalog supports them. Stop on an unexpected dialog and observe it before continuing.
