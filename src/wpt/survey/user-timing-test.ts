// Survey config.
import { type TestRunnerConfig } from 'harness/harness';

export default {
  "buffered-flag.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "case-sensitivity.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "clear_all_marks.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "clear_all_measures.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "clear_non_existent_mark.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "clear_non_existent_measure.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "clear_one_mark.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "clear_one_measure.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "entry_type.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "idlharness.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "invoke_with_timing_attributes.worker.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "mark-entry-constructor.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "mark-errors.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "mark-l3.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "mark-measure-return-objects.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "mark.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "measure-l3.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "measure-with-dict.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "measure_syntax_err.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "performance-measure-invalid.worker.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "structured-serialize-detail.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "supported-usertiming-types.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }},
  "user_timing_exists.any.js": {before: () => { performance.clearMarks(); performance.clearMeasures(); }}
} satisfies TestRunnerConfig;
