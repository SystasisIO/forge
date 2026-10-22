# Native expression comparison

Research results; not production qualification.

Integer expectations are compared exactly, without floating-point tolerance.
Native candidates use different number models; latency alone does not establish suitability.

| Backend | Exact matches | Mismatches | Harness errors |
| --- | ---: | ---: | ---: |
| cel | 67 | 2 | 0 |
| exprtk | 54 | 15 | 0 |

## Timed successful cases

Compile includes candidate preparation; evaluation reuses the prepared program and bound variables.
RSS is the whole child process peak, including libraries; it is not per-expression allocation.

| Case | Backend | Compile median, ns | Evaluate median, ns/op | Process RSS, bytes |
| --- | --- | ---: | ---: | ---: |
| lookup_1080_true | cel | 124208 | 379.1 | 10633216 |
| lookup_1080_true | exprtk | 58834 | 11.0 | 3473408 |
| tts_201 | cel | 99000 | 225.4 | 10600448 |
| tts_201 | exprtk | 60916 | 3.6 | 3375104 |
| seedance_15_11 | cel | 133542 | 217.5 | 10534912 |
| seedance_15_11 | exprtk | 46209 | 4.0 | 3375104 |
| llm_101_1 | cel | 124375 | 319.6 | 10600448 |
| llm_101_1 | exprtk | 55000 | 5.3 | 3342336 |
| wavespeed_1000_1000_1000 | cel | 316500 | 1245.0 | 10698752 |
| wavespeed_1000_1000_1000 | exprtk | 55208 | 11.8 | 3686400 |
| bitshares_data_1025 | cel | 86834 | 85.7 | 10534912 |
| bitshares_data_1025 | exprtk | 44417 | 2.4 | 3260416 |
| bitshares_scale_3_5000 | cel | 89167 | 82.7 | 10469376 |
| bitshares_scale_3_5000 | exprtk | 41125 | 2.3 | 3244032 |
| bitshares_asset_4 | cel | 102875 | 166.3 | 10551296 |
| bitshares_asset_4 | exprtk | 41500 | 3.3 | 3276800 |
| bitshares_staged | cel | 157708 | 363.0 | 10567680 |
| bitshares_staged | exprtk | 58291 | 6.8 | 3424256 |

## Mismatches

| Case | Backend | Actual | Classification |
| --- | --- | --- | --- |
| wavespeed_empty | exprtk | compile_error: 0 | expected_value_was_not_returned |
| wavespeed_2001_2001 | exprtk | compile_error: 0 | expected_value_was_not_returned |
| wavespeed_2499_2501 | exprtk | compile_error: 0 | expected_value_was_not_returned |
| wavespeed_1_14999 | exprtk | compile_error: 0 | expected_value_was_not_returned |
| wavespeed_7500_7500 | exprtk | compile_error: 0 | expected_value_was_not_returned |
| bitshares_scale_1000000000000000_9999 | cel | evaluation_error: 0 | expected_value_was_not_returned |
| identity_above_2pow53 | exprtk | ok: 9007199254740992 | exact_numeric_mismatch |
| cancellation_2pow53 | exprtk | ok: 0 | exact_numeric_mismatch |
| add_int64_overflow | exprtk | ok: 9.2233720368547758e+18 | expected_error_was_not_reported |
| multiply_int64_overflow | exprtk | ok: 9.2233720370002493e+18 | expected_error_was_not_reported |
| divide_int64_overflow | exprtk | ok: 9.2233720368547758e+18 | expected_error_was_not_reported |
| divide_zero | exprtk | ok: inf | expected_error_was_not_reported |
| integer_division | exprtk | ok: 3.5 | exact_numeric_mismatch |
| wide_intermediate | cel | evaluation_error: 0 | expected_value_was_not_returned |
| comprehension_1024 | exprtk | ok: 1 | expected_error_was_not_reported |
| comprehension_1025 | exprtk | ok: 1 | expected_error_was_not_reported |
| lookup_unknown | exprtk | ok: nan | expected_error_was_not_reported |

Cache hits, repeated inputs and the current host do not establish cross-platform determinism.
Linux, contract intrinsics, Spine/WAMR and financial settlement are outside this run.
