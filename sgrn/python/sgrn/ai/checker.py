"""
sgrn.ai.checker — Schema-Driven Tiered Safety Checker (T1/T2 Validation Layer).

Validates LLM action proposals against SCL schema rules (T1: range/enum/read-only)
and declarative interlocks (T2: plant mode permissives, E-stops) before execution.
"""

from dataclasses import dataclass
from typing import Dict, Any, Callable, Optional, Tuple, List, Union
from sgrn.semantic import SemanticSchema, FieldMetadata
from sgrn.ai.doer import ActionProposal


# A relational interlock is either the legacy 5-tuple
#     (cond_var, cond_val, target_var, invalid_val, message)
# which fires when ``current_state[cond_var] == cond_val`` (or ``cond_val is
# True`` and the value is truthy), or the richer 6-tuple
#     (cond_var, cond_predicate, target_var, message)
# where ``cond_predicate(state) -> bool`` expresses conditions equality cannot,
# such as "dew point is inside spec" or "a bed is mid-regeneration".
Interlock = Union[Tuple[str, Any, str, Any, str], Tuple[str, Any, str, str]]

_MISSING = object()


@dataclass
class ValidationResult:
    approved: bool
    violated_rule: Optional[str] = None
    tier: Optional[str] = None
    explanation: Optional[str] = None

    def toJson(self) -> str:
        import json
        return json.dumps({
            "approved": self.approved,
            "violated_rule": self.violated_rule,
            "tier": self.tier,
            "explanation": self.explanation
        })


class SchemaChecker:
    """Tiered Safety Checker enforcing T1 signal constraints and T2 relational interlocks."""

    def __init__(self, schema: SemanticSchema):
        self.schema = schema
        # Append process-specific interlocks here. See `Interlock` for the two
        # accepted shapes.
        self.relational_interlocks: List[Interlock] = []
        self._init_default_interlocks()

    def _init_default_interlocks(self) -> None:
        """Default T2 process interlocks for plant safety envelopes."""
        # Rule: If Supervisor.plant_estop is True (1), plant_mode cannot be set to RUNNING (2)
        self.relational_interlocks.append((
            "Supervisor.plant_estop", True, "Supervisor.plant_mode", 2,
            "T2 Interlock Violation: Cannot set plant_mode to RUNNING while plant_estop is ACTIVE"
        ))

    def validate_action(
        self, proposal: ActionProposal, current_state: Dict[str, Any]
    ) -> ValidationResult:
        field_key = f"{proposal.target_db}.{proposal.field}"

        if not self.schema.fields:
            return ValidationResult(False, "T1_EMPTY_SCHEMA", "T1",
                                    "No schema fields loaded; refusing to approve any action")

        meta = self.schema.get_field(proposal.target_db, proposal.field)
        if meta is None:
            return ValidationResult(False, f"T1_UNKNOWN_FIELD ({field_key})", "T1",
                                    f"Field '{field_key}' is not defined in the SCL schema")

        is_valid, err_msg = meta.validate_value(proposal.value)
        if not is_valid:
            return ValidationResult(False, f"T1_SIGNAL_CONSTRAINT ({field_key})", "T1", err_msg)

        # Normalise enum strings to their numeric index so interlocks compare like with like.
        value = proposal.value
        if isinstance(value, str) and meta.enum_map:
            value = meta.enum_map[value]

        for rule in self.relational_interlocks:
            if len(rule) == 5:
                cond_var, cond_val, target_var, invalid_val, rule_msg = rule
                if field_key != target_var or value != invalid_val:
                    continue
                current = current_state.get(cond_var, _MISSING)
                fired = current is _MISSING or current == cond_val or (cond_val is True and bool(current))
            else:
                cond_var, cond_predicate, target_var, rule_msg = rule
                if field_key != target_var:
                    continue
                try:
                    fired = bool(cond_predicate(current_state))
                except Exception:
                    fired = True  # cannot evaluate the permissive -> block
            if fired:
                return ValidationResult(
                    False, f"T2_RELATIONAL_INTERLOCK ({cond_var} -> {target_var})", "T2", rule_msg
                )

        return ValidationResult(
            True, None, "APPROVED",
            f"Action proposal '{field_key} = {proposal.value}' verified against all T1/T2 constraints."
        )
