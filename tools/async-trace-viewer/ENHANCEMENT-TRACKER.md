# Async Trace Viewer Enhancement Tracker

This document tracks implemented and potential enhancements for the async-trace-viewer tool.

## Completed Enhancements

### Parallelism View
- [x] Hover/click interaction - hover shows tooltip with details, click populates sidebar
- [x] Sync vs async distinction - solid bars for executing, faded for waiting
- [x] Bottleneck identification - identifies resources causing serialization
- [x] Critical path overlay - orange line shows critical path when enabled
- [x] Ideal parallelism comparison - cyan dashed line shows theoretical ideal
- [x] Efficiency metrics sidebar - shows utilization, efficiency, bottleneck stats

### Breakdown View
- [x] Multiple grouping modes - Type, Classification, Trigger Chain, Stack Location
- [x] Drill-down capability - click items with count > 1 to drill into details
- [x] Breadcrumb navigation - shows drill path, click to navigate back
- [x] Hover tooltips - shows full name, count, times, wall-clock attribution
- [x] Critical path highlighting - 🔥 indicator when critical path enabled
- [x] Bottleneck indicators - ⚠ indicator when bottleneck detection enabled
- [x] Treemap/Sunburst toggle - switch between visualization modes
- [x] Drill-down indicators - ▶ shows which items can be drilled into
- [x] Shift+R reset - resets drill-down path to top level
- [x] Wall-clock time attribution - shows actual time contribution

### Replay View (Previously Completed)
- [x] Event markers on timeline
- [x] Lifecycle badges (⏳⚡✓)
- [x] Loop mode (Shift+L)
- [x] Ghost mode for state changes (Shift+O)
- [x] Critical path highlighting
- [x] Waiting count badges
- [x] Trail effects for execution

## Potential Future Enhancements

### Latency View
- [x] Hover/click interaction - hover shows tooltip with latency range, count, type breakdown; click shows resources in sidebar
- [x] Cumulative distribution (CDF) toggle - switch between histogram and CDF view
- [x] Outlier detection - auto-identify statistical outliers (>3 std dev), highlight with distinct border
- [x] Critical path integration - highlight critical path resources in histogram when enabled
- [ ] Filter by resource type - clickable legend to toggle types on/off
- [ ] Latency comparison by type - separate mini-histograms per type or overlay with transparency
- [ ] Zoom/range selection - click and drag to zoom into latency range, Shift+R to reset
- [ ] Resource list panel - sidebar showing resources sorted by latency, click to navigate

### Gaps View
- [x] Hover/click interaction - hover shows tooltip with gap details, click shows waiting resources in sidebar
- [x] Gap classification - categorize gaps by cause (I/O, timer, fetch, etc.) with toggle
- [x] Critical path integration - highlight gaps on critical path when enabled
- [x] Threshold controls - adjustable minimum gap threshold, show/hide minor gaps toggle
- [x] Optimization recommendations - auto-detect patterns and suggest fixes with toggle
- [ ] Gap-to-resource linking - show which operation ends each gap, click to jump
- [ ] Gap timeline detail - zoom into gap to see detailed resource states
- [ ] Comparison mode - show ideal timeline if gaps eliminated, potential savings

### Heatmap View
- [ ] Hover for details at specific cells
- [ ] Click to filter/navigate
- [ ] Adjustable time bucket size
- [ ] Multiple color schemes

### General
- [ ] Cross-view linking - selections sync across all views
- [ ] Export/share functionality
- [ ] Comparison mode - diff two traces
- [ ] Keyboard navigation within views
