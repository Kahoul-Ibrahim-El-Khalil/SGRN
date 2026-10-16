"""
sgrn.ai — Agentic AI & Trustworthy Doer-Checker Pipeline.

EXPERIMENTAL SKETCH — not part of the delivered typed-twin research
path. The doer / checker / AngelScript compiler here prototype a
write-path; the fail-closed checker below is a prerequisite for any
"trustworthy AI" claim, and network LLM providers must be treated as
external data disclosures (see LLMDoer).
"""

from sgrn.ai.doer import LLMDoer, MockDoer, PlantDoer
from sgrn.ai.checker import SchemaChecker, ValidationResult
from sgrn.ai.compiler import ActionCompiler

__all__ = ["LLMDoer", "MockDoer", "PlantDoer", "SchemaChecker", "ValidationResult",
           "ActionCompiler"]