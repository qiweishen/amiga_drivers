# SOPAS ET capture: filter status-byte polarity

`app_config.h` sets `LFPfrontendEdgefilter` and `LFPcubicareafilter` to
`0x00` to turn them OFF. The manual's telegram tables say the opposite, so the
evidence for that choice lives here rather than in a comment — a future reader
who checks only the tables would "fix" it the wrong way and silently enable two
irreversible filters on every recording.

## What the manual says

*Table 130, "sWN LFPfrontendEdgefilter", p.105* and *Table 146,
"sWN LFPcubicareafilter", p.109* both print:

| Status code | Meaning |
|---|---|
| 0 | Activate |
| 1 | Deactivate |

## Why that cannot be right

The manual contradicts itself one page later. *Table 147, p.110* is headed
"Activate Cubic Area filter with area from 1 m … 2 m and width from -1.5 m …
+1.5 m" and the telegram it prints is:

```
sWN LFPcubicareafilter 1 +10000 +20000 -15000 +15000
                       ^ status byte = 1 = "Activate"
```

The other filters (`LFPmeanfilter` p.103, `LFPmedianfilter` p.104,
`LFPedgefilter` p.106, `LFPglossfilter` p.112) all document `0 = Inactive /
1 = Active`, i.e. the ordinary polarity. Only these two tables are inverted,
which reads like a copy/paste error in the document rather than two features
that genuinely behave backwards.

## What the device is actually sent

Captured from SOPAS ET 2018.2 against an LMS4124R-13000S01 while toggling each
filter's checkbox, CoLa B on TCP 2111. Bytes shown from the command name
onwards; `20` is the separator space, the last byte is the XOR checksum.

```
Enable   LFPfrontendEdgefilter   ... 4C 46 50 66 ... 65 72 20 01 01 1D
Disable  LFPfrontendEdgefilter   ... 4C 46 50 66 ... 65 72 20 00 01 1C

Enable   LFPcubicareafilter      ... 4C 46 50 63 ... 65 72 20 01 00 00 00 00 ...
Disable  LFPcubicareafilter      ... 4C 46 50 63 ... 65 72 20 00 00 00 00 00 ...
```

**01 = enable, 00 = disable** for both, matching every other filter and matching
the manual's own worked example.

## Conclusion

`ScanFixed::kFrontendEdgeFilterOff == 0x00` and
`ScanFixed::kCubicAreaFilterOff == 0x00` are correct. The bytes are pinned by
`tests/test_command_builder.cpp` ("The driver's fixed frames have the documented
structure"), so a change to either constant fails the test rather than the field.
