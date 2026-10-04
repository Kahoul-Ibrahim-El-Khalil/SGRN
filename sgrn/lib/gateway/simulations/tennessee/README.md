# Tennessee Eastman Process (TEP) — SGRN simulation

Surrogate of the Downs & Vogel (1993) benchmark, mapped onto the SGRN
information model (gateway twin + historian + ML pipeline).

## P&ID (simplified)

```
     A,C,D,E feeds            Reactor (DB1)  P~2700kPa T~120C L~75%
     ─────────────┐          ┌──────────────────────────┐
                  └─────────►│  feeds + reaction A-H    │──► product gas ──► Condenser/Separator (DB2)
                             │  cooling water (XMV 10)  │                   P~2600kPa L~50%
                             └──────────────────────────┘                         │ underflow (XMV 7)
                    ┌── recycle ◄── Compressor (DB4) ◄── vapor ────────────────────┘
                    │   work ~280 kW (XMV 5)                purge (XMV 6, analyzed XMEAS 29-36)
                    └── liquid ──► Stripper (DB3, steam XMV 9) ──► product (XMV 8, analyzed XMEAS 37-41)
PlantWide (DB5): fault_code, production/cost proxies, safety envelope, agent action audit.
```

## Tag table (52 process tags + plant metadata)

| SGRN twin path | TE tag | Nominal | Unit | Notes |
|---|---|---|---|---|
| `Reactor.a_feed_flow` | XMEAS 1 | 0.25 | kscm | stream 1 |
| `Reactor.d_feed_flow` | XMEAS 2 | 63 | kg/h | stream 2 |
| `Reactor.e_feed_flow` | XMEAS 3 | 54 | kg/h | stream 3 |
| `Reactor.ac_feed_flow` | XMEAS 4 | 0.25 | kscm | stream 4 |
| `Reactor.reactor_feed_rate` | XMEAS 6 | 42 | kscm | stream 6 |
| `Reactor.pressure` | XMEAS 7 | 2700 | kPa | safety-critical, checker watches 3000 |
| `Reactor.level` | XMEAS 8 | 75 | % | |
| `Reactor.temp` | XMEAS 9 | 120 | degC | checker watches 150 |
| `Reactor.cooling_out_temp` | XMEAS 21 | 92 | degC | |
| `Reactor.feed_comp.*` | XMEAS 23-28 | — | mol% | A-H in stream 6 |
| `Separator.purge_rate` | XMEAS 10 | 0.34 | kscm | stream 9 |
| `Separator.product_temp` | XMEAS 11 | 83 | degC | |
| `Separator.sep_level` | XMEAS 12 | 50 | % | |
| `Separator.sep_pressure` | XMEAS 13 | 2600 | kPa | |
| `Separator.sep_underflow` | XMEAS 14 | 26 | m3/h | stream 10 |
| `Separator.cooling_out_temp` | XMEAS 22 | 88 | degC | |
| `Separator.purge_comp.*` | XMEAS 29-36 | — | mol% | A-H in purge |
| `Stripper.level/pressure/underflow/temp/steam_flow` | XMEAS 15-19 | 50/2600/25/66/230 | %/kPa/m3/h/degC/kg/h | |
| `Stripper.product_comp.*` | XMEAS 37-41 | — | mol% | D-H in product |
| `Compressor.recycle_flow/work` | XMEAS 5/20 | 32/280 | kscm/kW | |
| `*.valve` | XMV 1-11 | — | % | manipulated valves |
| `PlantWide.fault_code` | IDV | 0 | — | fault selector, writable live |

XMV 12 (agitator) is held constant at 100%.

## Fault subset (demo IDs → TE IDVs)

| `fault_code` | TE IDV | Type | Signature in surrogate |
|---|---|---|---|
| 0 | 0 | normal | regulation to nominal + noise |
| 1 | 1 | step | A/C feed ratio step |
| 2 | 2 | step | B composition step |
| 3 | 3 | step | D feed temperature step |
| 4 | 4 | step | reactor cooling water inlet step |
| 5 | 6 | step | A feed loss |
| 6 | 7 | step | C header pressure loss |
| 7 | 8 | random | A/B/C feed composition variation |
| 8 | 11 | random | reactor cooling water variation |

IDs 9+ (unseen: sensor bias/drift, combined faults) are reserved for the
robustness / OOD tests in `demos/model.py` and are *not* in the training set.

## Run with SGRN

```bash
# Gateway + soft-PLC (needs built binaries, port 102 requires root):
sudo .dist/linux-static-release/gateway sgrn/lib/gateway/simulations/tennessee/gateway.json --gui
# in another shell, from the simulation dir:
.dist/linux-static-release/s7shell simulation.as

# Inject a fault live (checker demo):
curl -X POST http://localhost:8000/data/PlantWide \
  -H 'Content-Type: application/json' -d '{"fault_code": 4}'
```

Offline / without hardware (used by the EPFL demo):

```bash
micromamba run -n SGRN python demos/model.py --mode full
```

This generates the same tags synthetically, trains the trustworthy-AI
baseline (detector + diagnoser + soft-sensor + doer-checker), exports
`scratch/te_demo/{dataset.csv,manifest.json,model_meta.json,report.md}`,
and — if a gateway is reachable — posts predictions to the twin.

> Threshold note: the PCA detector threshold shipped in `demos/model.py` is
> calibrated on synthetic runs. Before closing the loop on live `s7shell`
> historian data, recalibrate it on a fault-free historian window
> (same procedure, see `train_bundle`: fit on commissioning run, threshold
> at 99th percentile of an independent normal run).
