# examples — runnable SGRN typed-twin workflows

## `twin_workflow.py`

Archive → model → time inference → feature prediction → streaming, with no
gateway, no SCL file and no CSV intermediate:

```bash
python3 examples/twin_workflow.py [--epochs N] [--workdir DIR]
```

It synthesizes a heater-skid JSONL archive (nominal RUNNING/STOPPED regimes
plus a stuck-valve fault: hot metal while the heater reads OFF), then:

1. reads it straight into a DataFrame (`JsonlDatasetReader.readPandas`),
2. trains from the archive's own embedded schema
   (`fitManifoldPipeline(schemaSource=reader.schema_json)`),
3. infers plant state at a nominal and a fault timestamp (`inferStateAt`),
4. predicts `Heater.temp` from the other signals in both regimes
   (`predictFeature` + conformal interval),
5. repeats training by streaming in O(window) memory
   (`fitManifoldPipelineStream`) and compares thresholds,
6. scores the fault stretch (`evaluateManifoldSequence`).

Seeds are pinned, so the asserted verdicts
(`normal` → `abnormal` with both mode switches named) reproduce exactly.
