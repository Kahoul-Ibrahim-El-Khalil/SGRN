#!/usr/bin/env python3
"""SGRN single test interface — every suite, one entry point.

Tests are organized by the module they verify (gateway, scl, datastore,
s7shell, utils) and by kind:

  python — suites under tests/<module>/*.py, run in-process or against a
           spawned gateway fixture.
  ts     — TypeScript E2E over the live network API (HTTP/WS/OPC-UA/S7);
           each file tests overall behavior of one surface.
  cpp    — native unit tests living next to their components
           (sgrn/lib/<module>/tests/), discovered from the CMake build and
           run individually via ctest.

Every entry's description is read from the test itself (Python docstring,
TS describe() title, leading C++ comment block); the registry below only
adds runner metadata (module, mode, args, assumptions).

Usage:
    python3 tests/run_tests.py                  # interactive menu
    python3 tests/run_tests.py <name> [sim]     # run one entry
    python3 tests/run_tests.py all [sim]        # run everything runnable
    python3 tests/run_tests.py --module gateway # run one module
    python3 tests/run_tests.py --kind ts         # run one test kind
    python3 tests/run_tests.py --list           # list entries + status
    python3 tests/run_tests.py <name> --message # describe one entry
"""
import ast
import os
import re
import sys
import subprocess
import argparse
import textwrap
import json
import time
import shutil
import urllib.request
import urllib.error
from contextlib import contextmanager

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TESTS_DIR)

BUILD_PRESETS = ("linux-static-release", "linux-static")
TS_DIR = os.path.join(TESTS_DIR, "ts")

MODULE_ORDER = ["gateway", "scl", "datastore", "s7shell", "utils", "suite"]

# ---------------------------------------------------------------------------
# Entry registry: runner metadata only. Human descriptions live in the test
# files themselves (see describe_* below) and are read at display time.
# Fields: module, kind (python|ts|cpp), mode (offline|integration|suite),
# path/file (kind-specific), args, env, simulation, needs_setup, assumptions.
# needs_setup entries are skipped by `all` (explicit runs only).
# ---------------------------------------------------------------------------
PYTHON_TESTS = [
    dict(name="gateway-rest", module="gateway", mode="offline",
         path="gateway/rest_api.py",
         assumptions="Runs a local HTTP server emulating the gateway. No live gateway needed."),
    dict(name="gateway-websocket", module="gateway", mode="integration",
         path="gateway/websocket.py",
         env={"SGRN_URL": "http://127.0.0.1:8080",
              "SGRN_WS_URL": "ws://127.0.0.1:8080/ws"},
         assumptions="Requires a live gateway with WebSocket enabled at /ws on the HTTP port (8080)."),
    dict(name="gateway-opcua", module="gateway", mode="integration",
         path="gateway/opcua_discovery.py",
         assumptions="Requires a live gateway with OPC UA enabled on port 4840."),
    dict(name="gateway-modbus", module="gateway", mode="integration",
         path="gateway/modbus.py", needs_setup=True,
         assumptions="Requires a live gateway with a Modbus mapping exposing the "
                     "DB17 registers the test drives (defaults: HTTP 8080, Modbus 5020; "
                     "override with --host/--http-port/--modbus-port). No in-tree "
                     "simulation provides that mapping today."),
    dict(name="gateway-enums", module="gateway", mode="integration",
         path="gateway/enum_tests.py", simulation="enums",
         args=["--http", "http://localhost:8080", "--opcua", "opc.tcp://localhost:4840"],
         assumptions="Requires the in-tree `enums` simulation and asyncua + requests packages."),
    dict(name="gateway-arrays", module="gateway", mode="integration",
         path="gateway/array_support.py", needs_setup=True,
         assumptions="Requires a gateway whose schema exposes the target array "
                     "(defaults: DB2.temperatures[10]; override with --db/--field/--array-size). "
                     "No in-tree simulation provides it today."),
    dict(name="gateway-live-snapshot", module="gateway", mode="integration",
         path="gateway/binary_live_snapshot.py",
         env={"SGRN_LIVE_EXTERNAL": "1",
              "SGRN_LIVE_GATEWAY_URL": "http://127.0.0.1:8080",
              "SGRN_LIVE_WS_URL": "ws://127.0.0.1:8080/ws"},
         assumptions="Heavy stress test. Runs in external mode against the fixture gateway."),
    dict(name="scl-dtypes", module="scl", mode="offline",
         path="scl/dtypes_endianness.py",
         assumptions="Offline. Depends on the sgrn/python package."),
    dict(name="scl-validation", module="scl", mode="offline",
         path="scl/schema_registry_validation.py", needs_sclc=True,
         assumptions="Offline. Requires the native `sclc` compiler binary to be built."),
]

# file -> (module, surface label). Descriptions come from describe() titles.
TS_TESTS = [
    ("tests/dashboard.test.ts", "gateway", "dashboard UI over HTTP"),
    ("tests/data.test.ts", "gateway", "data REST API"),
    ("tests/gateway-api.test.ts", "gateway", "REST API surface (registry/data/memory/policy)"),
    ("tests/policy.test.ts", "gateway", "policy engine API"),
    ("tests/registry.test.ts", "gateway", "schema registry API"),
    ("tests/websocket.test.ts", "gateway", "WebSocket telemetry API"),
    ("tests/s7shell.test.ts", "s7shell", "s7shell CLI binary"),
    ("tests/southbound-s7.test.ts", "gateway", "S7 southbound views over HTTP"),
    ("tests/southbound-modbus.test.ts", "gateway", "Modbus mapping views over HTTP"),
    ("tests/southbound-opcua.test.ts", "gateway", "OPC-UA views over HTTP/WebSocket"),
    ("tests/southbound-ethernetip.test.ts", "gateway", "EtherNet/IP views over HTTP/WebSocket"),
    ("tests/southbound-integration.test.ts", "gateway", "cross-protocol behavior (aggregation, errors, load)"),
]

TS_ASSUMPTIONS = "Requires Bun. Spawns its own isolated gateway/s7shell internally."


# ---------------------------------------------------------------------------
# Build dir + C++ test discovery
# ---------------------------------------------------------------------------
def detect_build_dir():
    best, best_count = None, -1
    for preset in BUILD_PRESETS:
        d = os.path.join(REPO_ROOT, ".build", preset)
        if not os.path.isfile(os.path.join(d, "CTestTestfile.cmake")):
            continue
        count = len(discover_cpp_tests(d))
        if count > best_count:
            best, best_count = d, count
    return best


def discover_cpp_tests(build_dir):
    """[(ctest_name, binary_path)] via ctest's machine-readable listing.

    Entries without a resolvable command (stale build trees that were
    reconfigured but never built) are skipped: they cannot run anyway.
    """
    try:
        out = subprocess.run(["ctest", "--show-only=json-v1", "--test-dir", build_dir],
                             capture_output=True, text=True, timeout=60)
        data = json.loads(out.stdout)
    except Exception:
        return []
    found = []
    for t in data.get("tests", []):
        cmd = t.get("command", [])
        binary = cmd[0] if isinstance(cmd, list) and cmd else None
        if not binary:
            continue
        if not os.path.isabs(binary):
            binary = os.path.join(build_dir, binary)
        found.append((t["name"], binary))
    return found


def cpp_module(binary_path):
    m = re.search(r"sgrn/lib/([^/]+)/", binary_path.replace(os.sep, "/"))
    return m.group(1) if m else "suite"


def cpp_source_for(test_name):
    """Locate sgrn/**/tests/<name>.cpp for the leading-comment description."""
    for root, dirs, _files in os.walk(os.path.join(REPO_ROOT, "sgrn")):
        if os.path.basename(root) != "tests":
            continue
        candidate = os.path.join(root, test_name + ".cpp")
        if os.path.isfile(candidate):
            return candidate
    return None


# ---------------------------------------------------------------------------
# Descriptions, read from the tests themselves
# ---------------------------------------------------------------------------
def describe_python(rel_path):
    try:
        with open(os.path.join(TESTS_DIR, rel_path)) as f:
            doc = ast.get_docstring(ast.parse(f.read())) or ""
    except Exception:
        return ""
    first = doc.strip().split("\n\n")[0].replace("\n", " ")
    return re.sub(r"\s+", " ", first).strip()


def describe_ts(rel_file):
    try:
        with open(os.path.join(TS_DIR, rel_file)) as f:
            text = f.read(4000)
    except Exception:
        return ""
    m = re.search(r'describe\(\s*["\'](.+?)["\']', text)
    return m.group(1).strip() if m else ""


def describe_cpp(test_name):
    src = cpp_source_for(test_name)
    if not src:
        return ""
    try:
        with open(src) as f:
            lines = f.read().splitlines()[:60]
    except Exception:
        return ""
    # First contiguous `//` block (headers live above or just below includes).
    comment = []
    for line in lines:
        s = line.strip()
        if s.startswith("//"):
            comment.append(s[2:].strip())
        elif not s or s.startswith("#"):
            continue
        else:
            break
    first = " ".join(c for c in comment if c).strip().split(". ")[0]
    return re.sub(r"\s+", " ", first).strip()


# ---------------------------------------------------------------------------
# Registry assembly
# ---------------------------------------------------------------------------
def build_registry():
    entries = {}
    order = []

    def add(entry):
        entries[entry["name"]] = entry
        order.append(entry["name"])

    for spec in PYTHON_TESTS:
        entry = dict(spec)
        entry["kind"] = "python"
        add(entry)

    for rel_file, module, surface in TS_TESTS:
        short = os.path.splitext(os.path.basename(rel_file))[0]
        if short.endswith(".test"):
            short = short[: -len(".test")]
        add({"name": "ts-" + short, "module": module, "kind": "ts",
             "mode": "suite", "file": rel_file, "surface": surface,
             "assumptions": TS_ASSUMPTIONS})

    add({"name": "ts-suite", "module": "suite", "kind": "ts", "mode": "suite",
         "surface": "network API end-to-end (HTTP/WS/OPC-UA/S7)",
         "about": "Whole TypeScript E2E suite in one go.",
         "assumptions": TS_ASSUMPTIONS})

    build_dir = detect_build_dir()
    if build_dir:
        for name, binary in discover_cpp_tests(build_dir):
            add({"name": "cpp-" + name, "module": cpp_module(binary), "kind": "cpp",
                 "mode": "offline", "ctest": name, "binary": binary,
                 "assumptions": "Native unit test. No PLC hardware or network needed."})
    add({"name": "cpp", "module": "suite", "kind": "cpp", "mode": "suite",
         "surface": "in-process code (native unit tests)",
         "about": "All native C++ unit tests via CTest.",
         "assumptions": "Requires a configured CMake build dir. No PLC hardware or network needed."})
    return entries, build_dir


def entry_description(name, info):
    """Description from the test file itself; registry text is the fallback."""
    if info["kind"] == "python":
        doc = describe_python(info["path"])
        if doc:
            return doc
    elif info["kind"] == "ts" and "file" in info:
        title = describe_ts(info["file"])
        if title:
            return title
    elif info["kind"] == "cpp" and "ctest" in info:
        comment = describe_cpp(info["ctest"])
        if comment:
            return comment
    return info.get("about", info.get("surface", info["mode"]))


def gateway_binary(build_dir, app):
    return os.path.join(build_dir, "sgrn", "apps", app, app) if build_dir else None


def check_prereqs(name, info, build_dir):
    if info["kind"] == "ts":
        if shutil.which("bun") is None:
            return False, "bun not on PATH"
        for app in ("gateway", "s7shell"):
            binary = os.path.join(REPO_ROOT, ".build", "linux-static-release", "sgrn", "apps", app, app)
            if not os.path.isfile(binary):
                return False, f"missing {app} binary {binary} (build the linux-static-release preset first)"
        return True, ""
    if info["kind"] == "cpp":
        if build_dir is None:
            return False, "no configured CMake build dir (.build/<preset>)"
        if info["name"] == "cpp":
            return True, ""
        if not os.path.isfile(info["binary"]):
            return False, f"test binary not built: {info['binary']} (build first)"
        return True, ""
    # python
    if info.get("needs_sclc"):
        sclc = os.path.join(REPO_ROOT, ".prefix", "linux-static", "bin", "sclc")
        if not os.path.isfile(sclc):
            return False, f"missing sclc binary {sclc} (build first)"
        return True, ""
    if info["mode"] == "integration":
        gw = gateway_binary(build_dir, "gateway")
        if not gw or not os.path.isfile(gw):
            return False, "missing gateway binary (build first)"
        return True, ""
    return True, ""


@contextmanager
def GatewayFixture(simulation_name, build_dir):
    """Spawns the SGRN Gateway and its AngelScript simulation for integration tests."""
    sim_name = simulation_name or "gas_processing"
    ts = int(time.time() * 1000)
    state_dir = f"/tmp/gateway-state-{sim_name}-{ts}"
    repo_sim_dir = os.path.join(REPO_ROOT, "sgrn", "lib", "gateway", "simulations", sim_name)

    os.makedirs(state_dir, exist_ok=True)

    policy_path = os.path.join(state_dir, "security.as")
    test_config_path = os.path.join(state_dir, "gateway.json")

    repo_policy = os.path.join(repo_sim_dir, "security.as")
    if os.path.exists(repo_policy):
        shutil.copy2(repo_policy, policy_path)
    else:
        with open(policy_path, "w") as f:
            f.write("void setup() { http().allow(); opcua().allow(); websocket().allow(); }\n")

    config = {
        "listen": {
            "s7": {"port": 8102},
            "opcua": {"port": 4840},
            "http": {"port": 8080},
            # WebSocket shares the HTTP listener at /ws (same port).
            "websocket": {"port": 8080}
        },
        # Pin the gateway's own archive/state dir under /tmp: the product
        # default is CWD-relative ("./gateway-state") and must never land
        # in the source tree during tests.
        "state_dir": os.path.join(state_dir, "gateway-state"),
        "schema": os.path.join(repo_sim_dir, "schema.scl"),
        "security_script": policy_path
    }

    with open(test_config_path, "w") as f:
        json.dump(config, f, indent=2)

    gateway_bin = gateway_binary(build_dir, "gateway")
    s7shell_bin = gateway_binary(build_dir, "s7shell")
    print(f"Starting Gateway with simulation '{sim_name}'...")
    gateway_proc = subprocess.Popen([gateway_bin, test_config_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    sim_proc = None
    sim_script = os.path.join(repo_sim_dir, "simulation.as")
    if os.path.exists(sim_script) and os.path.isfile(s7shell_bin):
        print("Starting S7Shell simulation script...")
        sim_proc = subprocess.Popen([s7shell_bin, sim_script], cwd=repo_sim_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Wait for gateway to be ready
    ready = False
    for _ in range(50):
        try:
            res = urllib.request.urlopen("http://127.0.0.1:8080/endpoints", timeout=1)
            if res.getcode() == 200:
                ready = True
                break
        except Exception:
            pass
        time.sleep(0.1)

    if not ready:
        gateway_proc.kill()
        if sim_proc:
            sim_proc.kill()
        raise RuntimeError("Gateway failed to start or bind to port 8080.")

    print("Gateway is online. Running test...")
    try:
        yield
    finally:
        print("Tearing down Gateway...")
        gateway_proc.terminate()
        if sim_proc:
            sim_proc.terminate()
        gateway_proc.wait()
        if sim_proc:
            sim_proc.wait()
        shutil.rmtree(state_dir, ignore_errors=True)


def print_message(name, info, build_dir):
    print("\n==============================================")
    print(f" TEST: {name} [{info['module'].upper()}] ({info['kind'].upper()}/{info['mode'].upper()})")
    print("==============================================")
    print("\nDescription:")
    print(textwrap.indent(textwrap.fill(entry_description(name, info), width=70), "   "))
    if info.get("surface"):
        print(f"\nVerifies: {info['surface']}")
    print("\nAssumptions:")
    print(textwrap.indent(textwrap.fill(info.get("assumptions", "None."), width=70), "   "))
    loc = {"python": info.get("path"), "ts": info.get("file", "ts/"),
           "cpp": info.get("binary", "<ctest>")}[info["kind"]]
    print(f"\nLocation: {loc}")
    ok, reason = check_prereqs(name, info, build_dir)
    print(f"Status: {'ready' if ok else 'UNAVAILABLE — ' + reason}")
    print("==============================================\n")


def run_test(entries, build_dir, name, simulation_name=None):
    """Run one entry. Returns a (status, seconds) tuple; streams live output."""
    info = entries[name]
    started = time.time()
    # An entry-pinned simulation is a hard requirement (the test only makes
    # sense against it); the CLI positional sim is the fallback for the rest.
    sim = info.get("simulation") or simulation_name

    print("\n==============================================")
    print(f" Running {name} " + (f"(Simulation: {sim})" if sim else ""))
    print(f"  {info['module']}/{info['kind']}/{info['mode']}: {entry_description(name, info)}")
    print("==============================================\n")

    def _execute():
        if info["kind"] == "ts":
            cmd = ["bun", "test"] + ([info["file"]] if "file" in info else [])
            return subprocess.run(cmd, cwd=TS_DIR).returncode
        if info["kind"] == "cpp":
            if info["name"] == "cpp":
                cmd = ["ctest", "--test-dir", build_dir, "--output-on-failure"]
            else:
                cmd = ["ctest", "--test-dir", build_dir, "-R", f"^{info['ctest']}$", "--output-on-failure"]
            return subprocess.run(cmd, cwd=REPO_ROOT).returncode
        script_path = os.path.join(TESTS_DIR, info["path"])
        env = os.environ.copy()
        env.update(info.get("env", {}))
        if sim:
            env["SGRN_SIMULATION"] = sim
        return subprocess.run([sys.executable, script_path] + info.get("args", []), env=env).returncode

    try:
        if info["mode"] == "integration" and info["kind"] == "python":
            with GatewayFixture(sim, build_dir):
                returncode = _execute()
        else:
            returncode = _execute()
    except RuntimeError as e:
        print(f"{name} FAILED to start fixture: {e}")
        return ("error", time.time() - started)

    elapsed = time.time() - started
    status = "pass" if returncode == 0 else "fail"
    print(f"\n{name} {status.upper()} ({elapsed:.1f}s)")
    return (status, elapsed)


def parse_selection(choice, mapping, entries):
    """'1,3-5,all,cpp' -> ordered entry names. 'q'/empty -> []."""
    choice = choice.strip()
    if not choice or choice.lower() in ("q", "quit", "exit"):
        return []
    names = list(entries.keys())
    picked = []
    for token in choice.replace(",", " ").split():
        token = token.strip().lower()
        if token == "all":
            return names
        if token in entries and token not in picked:
            picked.append(token)
            continue
        if "-" in token:
            try:
                lo, hi = token.split("-", 1)
                for i in range(int(lo), int(hi) + 1):
                    if i in mapping and mapping[i] not in picked:
                        picked.append(mapping[i])
                continue
            except ValueError:
                pass
        if token.isdigit() and int(token) in mapping:
            if mapping[int(token)] not in picked:
                picked.append(mapping[int(token)])
            continue
        print(f"Unknown selection: {token}")
        return []
    return picked


def menu_order(entries):
    """Canonical display order: modules first, registry order within."""
    cats = {}
    for k, v in entries.items():
        cats.setdefault(v["module"], []).append(k)
    ordered = []
    for cat in MODULE_ORDER + [c for c in cats if c not in MODULE_ORDER]:
        ordered.extend(cats.get(cat, []))
    return ordered


def print_menu(entries, build_dir):
    """Grouped menu with per-entry readiness. Returns index -> name mapping."""
    print("\nSGRN Test Runner — tests grouped by the module they verify")
    print("python = suites under tests/<module> · ts = live network API E2E · cpp = native unit tests")
    print("-" * 70)
    mapping = {}
    prev_mod = None
    for idx, t in enumerate(menu_order(entries), start=1):
        info = entries[t]
        if info["module"] != prev_mod:
            print(f"\n[{info['module'].upper()}]")
            prev_mod = info["module"]
        ok, reason = check_prereqs(t, info, build_dir)
        tag = "ready" if ok else f"UNAVAILABLE ({reason})"
        desc = entry_description(t, info)
        short = (desc[:62] + "…") if len(desc) > 63 else desc
        print(f"  {idx:2d}. {t:34s} [{info['kind']:6s}/{info['mode']:11s}] {short}")
        print(f"      └─ {tag}")
        mapping[idx] = t
    print("-" * 70)
    return mapping


def print_summary(entries, results):
    """results: list of (name, status, seconds)."""
    print("\n==============================================")
    print(" RESULTS by module")
    print("==============================================")
    by_mod = {}
    for name, status, secs in results:
        by_mod.setdefault(entries[name]["module"], []).append((name, status, secs))
    totals = {"pass": 0, "fail": 0, "skip": 0, "error": 0}
    total_time = 0.0
    for mod in MODULE_ORDER + [m for m in by_mod if m not in MODULE_ORDER]:
        if mod not in by_mod:
            continue
        print(f"\n  [{mod}]:")
        for name, status, secs in by_mod[mod]:
            totals[status] += 1
            total_time += secs
            icon = {"pass": "PASS", "fail": "FAIL", "skip": "SKIP", "error": "ERROR"}[status]
            print(f"    {icon:5s} {name:34s} ({secs:.1f}s)")
    print(f"\n  {totals['pass']} passed, {totals['fail']} failed, "
          f"{totals['skip']} skipped, {totals['error']} errors in {total_time:.1f}s")
    print("==============================================\n")
    return totals["fail"] == 0 and totals["error"] == 0


def run_selection(entries, build_dir, names, simulation_name=None, include_setup=False):
    # Running an aggregate suite alongside its individual entries repeats work.
    if "ts-suite" in names and any(n.startswith("ts-") and n != "ts-suite" for n in names):
        names = [n for n in names if n != "ts-suite"]
    if "cpp" in names and any(n.startswith("cpp-") and n != "cpp" for n in names):
        names = [n for n in names if n != "cpp"]
    results = []
    for name in names:
        info = entries[name]
        if info.get("needs_setup") and not include_setup:
            print(f"\nSKIP {name} — needs a dedicated setup ({info.get('assumptions', '')})")
            results.append((name, "skip", 0.0))
            continue
        ok, reason = check_prereqs(name, info, build_dir)
        if not ok:
            print(f"\nSKIP {name} — {reason}")
            results.append((name, "skip", 0.0))
            continue
        status, secs = run_test(entries, build_dir, name, simulation_name)
        results.append((name, status, secs))
    return print_summary(entries, results)


def main():
    parser = argparse.ArgumentParser(description="SGRN single test interface: every suite, one entry point")
    parser.add_argument("test", nargs="?", help="Test name, number, 'all', or comma/range selection (e.g. 1,3-5)")
    parser.add_argument("simulation", nargs="?", help="Optional simulation name to pass")
    parser.add_argument("--message", action="store_true", help="Print the test description and assumptions instead of running it")
    parser.add_argument("--list", action="store_true", help="List all entries with readiness and exit")
    parser.add_argument("--module", dest="module", default=None,
                        help="Run all entries of one module (gateway, scl, datastore, s7shell, utils, suite)")
    parser.add_argument("--kind", choices=("python", "ts", "cpp"),
                        help="Limit selection to Python, TypeScript, or C++ tests")
    args = parser.parse_args()

    entries, build_dir = build_registry()
    if args.kind:
        entries = {name: info for name, info in entries.items() if info["kind"] == args.kind}

    if args.list:
        print_menu(entries, build_dir)
        return

    if args.message and args.test:
        if args.test == "all":
            for name in entries:
                print_message(name, entries[name], build_dir)
        elif args.test in entries:
            print_message(args.test, entries[args.test], build_dir)
        else:
            print(f"Unknown test: {args.test}")
        return
    elif args.message:
        print("Please specify a test name or 'all' with --message flag.")
        return

    if args.module:
        names = [n for n in entries if entries[n]["module"] == args.module]
        if not names:
            print(f"Unknown module: {args.module} (try: {', '.join(MODULE_ORDER)})")
            sys.exit(1)
        ok = run_selection(entries, build_dir, names, args.simulation, include_setup=True)
        sys.exit(0 if ok else 1)

    if not args.test:
        mapping = print_menu(entries, build_dir)
        try:
            choice = input("Select (numbers/ranges/names, 'all', or 'q'): ").strip()
        except EOFError:
            print()
            return
        if choice.endswith("--message"):
            args.message = True
            choice = choice.replace("--message", "").strip()
        names = parse_selection(choice, mapping, entries)
        if args.message:
            for name in names or entries.keys():
                print_message(name, entries[name], build_dir)
            return
        if not names:
            return
        # The literal `all` means "everything runnable" (setup-gated entries
        # stay skipped); any other explicit pick runs what was picked.
        include_setup = choice.strip().lower() != "all"
        if any(entries[n]["mode"] == "integration" and entries[n]["kind"] == "python" for n in names) \
                and not args.simulation:
            try:
                sim = input("Simulation for integration tests (Enter for default): ").strip()
            except EOFError:
                sim = ""
                print()
            if sim:
                args.simulation = sim
        # Interactive picks are explicit: run setup-gated entries too.
        ok = run_selection(entries, build_dir, names, args.simulation, include_setup=include_setup)
        sys.exit(0 if ok else 1)

    if args.test == "all":
        ok = run_selection(entries, build_dir, list(entries.keys()), args.simulation)
        sys.exit(0 if ok else 1)
    elif args.test in entries:
        ok = run_selection(entries, build_dir, [args.test], args.simulation, include_setup=True)
        sys.exit(0 if ok else 1)
    else:
        # maybe a bare selection like "1,3" passed positionally (menu numbering)
        mapping = {i + 1: n for i, n in enumerate(menu_order(entries))}
        names = parse_selection(args.test, mapping, entries)
        if not names:
            print(f"Unknown test: {args.test}")
            sys.exit(1)
        # Explicit selection runs everything picked, including setup-gated entries.
        ok = run_selection(entries, build_dir, names, args.simulation, include_setup=True)
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
