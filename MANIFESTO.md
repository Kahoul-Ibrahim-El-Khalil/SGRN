# The SGRN Manifesto

## Identifying the Systemic Bottleneck

In the domain of industrial data engineering, the primary obstacle to scalable data aggregation is rarely the limitation of the storage layer itself. Instead, 
the critical bottleneck is the extraction of data from the industrial plant. The prevailing paradigm treats every Programmable Logic Controller (PLC) as a bespoke integration target, 
necessitating custom, protocol-specific adapters. Scalability—the ostensible objective—is thus rendered mathematically intractable, not due to database constraints, but because the upstream data ingestion pipeline lacks a generalized, deterministic architecture.

Concurrently, automation engineers face a mirrored manifestation of this systemic flaw. A routine modification to a PLC's memory layout inevitably breaks downstream, hand-coded middleware, necessitating manual recalculation of byte offsets and imposing arbitrary vendor licensing fees per connection or per tag.

Both paradigms are currently accepted as the immutable cost of operating in the industrial data sector. We reject this premise as empirically false and architecturally unsound.

## Core Architectural Axioms

**Data Collection at Scale is the Primary Objective; PLC Integration is Merely the Preceding Obstacle.**
This initiative was not conceived to proliferate PLC tooling as an end in itself. Its genesis was the requirement to collect and persist industrial data reliably and at scale. Achieving this required the systemic elimination of the primary impediment: manual, per-PLC integration tasks.

Every component within this technology stack—excluding the datastore itself (i.e., the gateway, the schema compiler, the soft-PLC)—functions explicitly to serve this singular objective. The schema-driven automation constitutes the methodology; reliable, scalable data collection constitutes the teleological end.

**The Schema is the Uncontested Source of Truth.**
If an automation engineer defines the information model within the PLC—via SCL (Structured Control Language) or native Siemens datablock exports—that declarative definition must be sufficient. The architecture strictly prohibits the maintenance of secondary tag databases or manually authored protocol translation layers. By parsing the pre-existing schema, all downstream transmission and upstream reception components can deterministically regenerate their required interfaces.

**Determinism is a Non-Negotiable Constraint.**
A gateway that introduces unpredictable latency via garbage collection pauses, or exhibits unbound memory allocation over prolonged operational cycles, ceases to be a mere inconvenience; it becomes a critical liability when deployed adjacent to physical machinery. The selection of C++, RAII (Resource Acquisition Is Initialization), and deterministic memory arenas was motivated by the axiom that "probabilistic correctness" is an unacceptable standard for industrial automation.

**Total Isolation of the Automation Controller.**
The fundamental mandate of a PLC is to execute deterministic control loops. Information Technology (IT) systems possess no inherent entitlement to compromise this determinism through aggressive polling. Consequently, every read operation initiated by an HTTP or OPC-UA client must interface with a localized, in-memory shadow of the PLC state—never directly with the controller itself. In this context, isolation is not merely a performance optimization; it is a rigid architectural boundary.

**Economic Constraints are Implementation Artifacts, Not Natural Laws.**
Per-tag licensing models persist because they are commercially viable, not technically mandated. We reject the paradigm where automation engineers or data engineers are precluded from implementing data-driven maintenance due to prohibitive cost structures. This stack is provided as free, self-contained, and AGPL-licensed software—not as an abstract ideological statement, but as a pragmatic mechanism to eliminate artificial barriers to entry.

**Abstracting the Pipeline from the Process.**
The declarative promise of this architecture is explicit: an engineer should allocate cognitive resources to determining _what_ data is required and its semantic nomenclature, rather than determining _where_ it resides, its structural composition, and its routing mechanisms. Every component, from the PLC memory layout to the datastore schema, is engineered to facilitate this abstraction.

**Automate Repetitive Labor, Preserve Engineering Judgment.**
Offset calculation, interface generation, and protocol translation require zero engineering judgment and should therefore consume zero engineering time. `scl` parses the schema once, generating the requisite C++ headers for non-S7 clients, ensuring correctness by construction rather than convention. The gateway and `s7shell` share this generated model, precluding state drift. The architecture automates deterministic bookkeeping, reserving human intellect exclusively for process definition and anomaly resolution.

**Strongly Typed Control Vectors.**
A process tag is not a loosely typed string coupled with a byte offset; it is a strongly typed primitive (e.g., `Real`, `Bool`, `DTL`) as declared by the schema. `s7shell` provides a scriptable simulation environment that exposes this typed model directly to control logic. Type mismatches surface as rigid compile-time or parse-time errors, ensuring that structural incompatibilities are caught deterministically prior to interacting with the physical controller.

This typed fidelity extends inherently to advanced simulation. Because `scl` generates AngelScript-bound types directly from the schema, the simulation environment speaks the precise information model of the production environment. Thus, a multi-client, multi-instance simulation framework is a direct corollary of refusing to bifurcate the definition of a process tag between control and simulation contexts.

**Schema-Driven Architecture at All Boundaries.**
The system is neither schema-_informed_ nor schema-_assisted_—it is entirely schema-_driven_. The information model is declared precisely once in the native PLC export. Every subsequent interface (REST, WebSocket, OPC-UA, generated C++ headers) is a deterministic projection of that singular declaration. Any divergence between interfaces constitutes a defect, not an edge case.

**Minimalism as an Architectural Constraint.**
Every external dependency and configuration variable introduces a failure vector for edge devices deployed in remote environments. We prioritize the distribution of self-contained, statically linked binaries over complex, loosely coupled microservice constellations. Every additional moving part increases the required maintenance and trust surface on the edge deployment.

**Performance and Scalability are Correctness Properties.**
The capacity to poll thousands of tags without saturating the PLC’s network stack is not a post-hoc optimization; it is a fundamental correctness requirement derived from the controller isolation principle. Zero-allocation data paths and shadow memory structures ensure that scalability is architecturally guaranteed, rather than empirically hoped for. The scalability of the storage layer is strictly bounded by the scalability of the collection layer.

## Architectural Patterns We Enforce (Beyond the PLC)

While this stack was built for industrial automation, the architectural constraints it enforces are universal. Stripped of the PLC-specific protocols, SGRN is fundamentally a high-concurrency, strictly typed, schema-driven data ingestion pipeline. We enforce the same patterns that define modern, high-performance API development:

**Schema-Driven Generation (The gRPC / OpenAPI Pattern)**
Modern APIs rely on Protobuf or OpenAPI to ensure clients and servers never disagree on the shape of data. We enforce the exact same standard. `scl` treats the PLC's native schema as the absolute source of truth, generating C++ headers and memory maps directly from it. Hand-written glue code is a liability; generation from a strict schema is the cure.

**Zero-Allocation API Gateways**
High-traffic API gateways (like Envoy or Nginx) are built to route massive concurrency without garbage collection pauses. Our `gateway` is built the same way. Relying on C++ RAII and deterministic memory arenas isn't just about hardware constraints—it's how you guarantee that handling massive throughput won't result in memory bloat or latency spikes.

**Shadow Memory (The API Caching Pattern)**
We protect the physical controller by serving reads from a local shadow of its memory. In API design, this is the equivalent of an in-memory cache (like Redis). You do not query the slow, critical primary resource for every read. You serve it deterministically from an ultra-fast cache, isolating the core system from external load.

**Strictly Typed Payloads**
A major source of bugs in REST APIs is relying on loosely typed JSON payloads. We enforce strict data types (`Real`, `Bool`, `DTL`) at the boundaries. Much like GraphQL, we ensure that payloads have strict, enforced types before the system attempts to process them. A type mismatch is a hard error, not a silent misread.

## Scope and Limitations

This document does not assert that the industrial data problem is definitively solved; rather, it articulates a theoretical framework for its resolution and provides an empirical implementation validating that framework.

We do not claim this toolset is production-hardened. Currently, SGRN has not undergone rigorous hardware-in-the-loop testing, has not been fuzzed under saturated concurrent loads, nor has it completed longitudinal soak testing to empirically verify memory safety on embedded edge hardware. These criteria are prerequisites for production deployment and remain unsatisfied.

## Historical Context

This stack was not conceived as four simultaneous tools. The `gateway`, `s7shell`, and `scl` were successive precursors to the primary objective of scalable data collection (`datastore`). The full history is in [`ORIGINS.md`](./ORIGINS.md).

## Target Audience

If your operational context aligns with the target audience outlined in the [`README.md`](./README.md)—requiring scalable data collection without brittle integrations, eliminating manual offset calculations, and avoiding vendor lock-in—then the schema is yours. Define it, and the architecture will deterministically follow.
