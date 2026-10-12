import unittest
import sys
import os

repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(repo_root, "sgrn", "python"))

from sgrn import (
    SemanticSchema,
    FieldMetadata,
    ConformalAnomalyDetector,
    SplitConformalCalibrator,
    LLMDoer,
    MockDoer,
    SchemaChecker,
    ActionCompiler,
)
from sgrn.ai.doer import ActionProposal


class TestTrustworthyAIPipeline(unittest.TestCase):

    def setUp(self):
        self.schema_dict = {
            "data_blocks": [
                {
                    "number": 1,
                    "name": "Supervisor",
                    "fields": [
                        {"name": "plant_mode", "type": "USInt", "enum_map": {"STOPPED": 0, "STARTING": 1, "RUNNING": 2}, "description": "Mode"},
                        {"name": "master_speed_sp", "type": "Real", "unit": "%", "min_val": 0.0, "max_val": 100.0},
                        {"name": "plant_estop", "type": "Bool", "read_only": False}
                    ]
                }
            ]
        }
        self.schema = SemanticSchema(self.schema_dict)

    def test_semantic_schema(self):
        meta = self.schema.get_field("Supervisor", "master_speed_sp")
        self.assertIsNotNone(meta)
        self.assertEqual(meta.unit, "%")
        self.assertEqual(meta.min_value, 0.0)
        self.assertEqual(meta.max_value, 100.0)

        # Test T1 signal validation
        valid, err = meta.validate_value(75.0)
        self.assertTrue(valid)

        invalid, err = meta.validate_value(150.0)
        self.assertFalse(invalid)
        self.assertIn("exceeds maximum", err)

    def test_conformal_calibrator(self):
        calibrator = SplitConformalCalibrator(alpha=0.05)
        scores = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0]
        q_hat = calibrator.calibrate(scores)
        self.assertGreater(q_hat, 0.8)

    def test_schema_checker_t1_t2(self):
        checker = SchemaChecker(self.schema)
        
        # Valid proposal
        prop1 = ActionProposal(target_db="Supervisor", field="plant_mode", value=1, explanation="Normal start")
        res1 = checker.validate_action(prop1, {"Supervisor.plant_estop": False})
        self.assertTrue(res1.approved)

        # T1 Violation (Out of Range)
        prop2 = ActionProposal(target_db="Supervisor", field="master_speed_sp", value=200.0, explanation="Speed boost")
        res2 = checker.validate_action(prop2, {})
        self.assertFalse(res2.approved)
        self.assertIn("T1", res2.tier)

        # T2 Violation (E-stop active locking plant_mode = RUNNING)
        prop3 = ActionProposal(target_db="Supervisor", field="plant_mode", value=2, explanation="Force run")
        res3 = checker.validate_action(prop3, {"Supervisor.plant_estop": True})
        self.assertFalse(res3.approved)
        self.assertIn("T2", res3.tier)

    def test_action_compiler(self):
        prop = ActionProposal(target_db="Supervisor", field="plant_mode", value=1, explanation="Test")
        as_code = ActionCompiler.compile_to_angelscript(prop)
        ws_json = ActionCompiler.compile_to_websocket_json(prop, db_number=1)

        self.assertIn("Supervisor.plant_mode = 1;", as_code)
        self.assertIn('"field": "plant_mode"', ws_json)


if __name__ == "__main__":
    unittest.main()
