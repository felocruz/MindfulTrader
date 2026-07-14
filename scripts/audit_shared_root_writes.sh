#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

live_file="$repo_root/src/messaging/EventSerializer.cpp"
training_file="$repo_root/src/IndicatorManager.cpp"

if [[ ! -f "$live_file" || ! -f "$training_file" ]]; then
  echo "WS-07 audit error: expected source files were not found." >&2
  exit 1
fi

live_manual_pattern='event_builder\.add_(side|market_symbol|overnight_exit|nh_nl_daily|prev_high|prev_low|prev_day_high|prev_day_low|prev_four_bar_high|prev_four_bar_low|close_percentile|volume_ratio_percent|volume_imbalance)\('
training_manual_pattern='event->(side|market_symbol|overnight_exit|nh_nl_daily|prev_high|prev_low|prev_day_high|prev_day_low|prev_four_bar_high|prev_four_bar_low|close_percentile|volume_ratio_percent|volume_imbalance)\s*='

echo "WS-07 audit: checking shared-root writer ownership"

if rg --line-number "$live_manual_pattern" "$live_file"; then
  echo "WS-07 audit failed: manual live shared-root writes detected in src/messaging/EventSerializer.cpp" >&2
  exit 1
fi

if rg --line-number "$training_manual_pattern" "$training_file"; then
  echo "WS-07 audit failed: manual training shared-root writes detected in src/IndicatorManager.cpp" >&2
  exit 1
fi

if ! rg --quiet 'WriteEventRootSharedFields\(' "$live_file"; then
  echo "WS-07 audit failed: generated live shared writer call missing in src/messaging/EventSerializer.cpp" >&2
  exit 1
fi

if ! rg --quiet 'WriteTrainingRootSharedFields\(' "$training_file"; then
  echo "WS-07 audit failed: generated training shared writer call missing in src/IndicatorManager.cpp" >&2
  exit 1
fi

echo "WS-07 audit passed: shared-root writes are generated-writer owned"
