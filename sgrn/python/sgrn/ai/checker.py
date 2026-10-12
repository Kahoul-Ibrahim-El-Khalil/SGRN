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


@dataclass
class ValidationResult:
    approved: bool
    violated_rule: Optional[str] = None
    tier: Optional[str] = None
    explanation: Optional[str] = None

    def to_json(self) -> str:
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
        meta = self.schema.get_field(proposal.target_db, proposal.field)

        # ─── Tier 1 (T1): Signal-level Schema Constraints ────────────────────
        if meta:
            is_valid, err_msg = meta.validate_value(proposal.value)
            if not is_valid:
                return ValidationResult(
                    approved=False,
                    violated_rule=f"T1_SIGNAL_CONSTRAINT ({field_key})",
                    tier="T1",
                    explanation=err_msg
                )
        else:
            # If field is completely unknown to schema
            if self.schema.fields:  # only if schema was loaded
                return ValidationResult(
                    approved=False,
                    violated_rule=f"T1_UNKNOWN_FIELD ({field_key})",
                    tier="T1",
                    explanation=f"Field '{field_key}' is not defined in the SCL schema"
                )

        # ─── Tier 2 (T2): Relational Interlocks & Permissives ────────────────
        for rule in self.relational_interlocks:
            if len(rule) == 5:
                cond_var, cond_val, target_var, invalid_val, rule_msg = rule
                if field_key != target_var or proposal.value != invalid_val:
                    continue
                current = current_state.get(cond_var)
                fired = current == cond_val or (cond_val is True and bool(current))
            else:
                cond_var, cond_predicate, target_var, rule_msg = rule
                if field_key != target_var:
                    continue
                fired = bool(cond_predicate(current_state))
            if fired:
                return ValidationResult(
                    approved=False,
                    violated_rule=f"T2_RELATIONAL_INTERLOCK ({cond_var} -> {target_var})",
                    tier="T2",
                    explanation=rule_msg,
                )

        return ValidationResult(
            approved=True,
            tier="APPROVED",
            explanation=f"Action proposal '{field_key} = {proposal.value}' verified against all T1/T2 constraints."
        )
