# Evaluation report

Seeds 11, 12, 13: one simulated week each for a row of 6 racks (16 sensors, 2,900,907 sensor samples in total), run through the real envmond binary, collector and PostgreSQL.

## Alarm quality

| Alarm logic | Incidents due an alarm | Caught | Alarms raised | False alarms | Precision | Alarms per day |
|---|---|---|---|---|---|---|
| Plain thresholds (--no-hysteresis) | 66 | 66 (100.0%) | 1492 | 739 | 50.5% | 71.0 |
| Delays + hysteresis (default) | 66 | 64 (97.0%) | 105 | 6 | 94.3% | 5.0 |

Median time from the condition starting to the alarm: 140 s with delays, 0 s without.

## Delivery under failures

The collector was killed with SIGKILL 143 times and envmond was restarted 6 times with messages still in its spool. Of 484,305 messages issued, **0 were lost and 0 were stored twice**.

Input validation rejected 959 bad samples (probe error values, glitches, malformed lines). Throughput end to end: about 52,081 sensor samples per second.
