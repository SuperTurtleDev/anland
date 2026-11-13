# Audio recovery regressions

```sh
python3 -m unittest discover -s tests -v
```

The ten tests execute service functions with private command/PID fixtures and
compile the actual AAudio resume function against recording stubs. The resume
matrix includes eight start/state/open outcomes. Worker tests cover retry
bounds, server loss, default-sink selection, replacement-owner cleanup and stale
PID rejection. Build the complete Android audio module and validate real route
changes/cold boot separately.
