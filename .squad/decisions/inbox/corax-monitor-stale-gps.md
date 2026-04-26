# Decision: Fix stale GPS display in lora_monitor.py

**Author:** Corax  
**Date:** 2026-04-26  
**Status:** Implemented  
**Addresses:** Perturabo Concern 5

## Problem

`tools/lora_monitor.py` GPS tab kept showing the last good coordinates even after a node lost its GPS fix (fix=0). The `_update_gps_tab()` method was only called when `fix=1`, so `fix=0` beacons never updated the GPS tab display.

## Decision

Always call `_update_gps_tab()` when a v5 beacon contains GPS fields, regardless of fix status. When fix=0:

- Show "✗ NO FIX" in the fix column (was just "✗")
- Prefix coordinates with `[stale]` so they're visually distinct
- Prefix maps link with `(stale)` 
- Grey out the row via the existing `nofix` tag (foreground="gray")
- Show "— no data —" if coordinates are 0,0

When fix=1:
- Show "✓ FIX" in green
- Display coordinates normally

## Additional change

Replaced the unused "ALT" column with "LAST FIX" — tracks the timestamp of the most recent valid fix per node, giving operators a quick sense of how long a node has been without GPS.

## Files changed

- `tools/lora_monitor.py`
