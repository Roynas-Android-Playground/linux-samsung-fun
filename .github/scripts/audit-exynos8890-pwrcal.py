#!/usr/bin/env python3
"""Enforce single-owner Exynos8890 DVFS after the PWRCAL cutover.

The audit deliberately reads files from the checkout, not ``git show HEAD``.
That makes it useful while a cutover is still being prepared and ensures CI
checks exactly the source that the compiler will consume.
"""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys


# The removed implementation consisted of 35 files plus its public header.
# Keep this explicit: a partial resurrection must be visible even if none of
# the restored files has a remaining in-tree caller yet.
LEGACY_TARGETS = frozenset(
    {
        "drivers/soc/samsung/pwrcal8890/Makefile",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-asv.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-ccf-bind.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-cmu.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-cmu.h",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-cmusfr.h",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-dfs.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-drampara.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-dvfs-clk.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-pll.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-pmu.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-pmusfr.h",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-rae.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-sfrbase.h",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-syspwr.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-vclk-internal.h",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-vclk.c",
        "drivers/soc/samsung/pwrcal8890/S5E8890/S5E8890-vclk.h",
        "drivers/soc/samsung/pwrcal8890/power-cal.c",
        "drivers/soc/samsung/pwrcal8890/pwrcal-asv.h",
        "drivers/soc/samsung/pwrcal8890/pwrcal-clk.h",
        "drivers/soc/samsung/pwrcal8890/pwrcal-cmu.c",
        "drivers/soc/samsung/pwrcal8890/pwrcal-dbg.c",
        "drivers/soc/samsung/pwrcal8890/pwrcal-dfs.c",
        "drivers/soc/samsung/pwrcal8890/pwrcal-dfs.h",
        "drivers/soc/samsung/pwrcal8890/pwrcal-dram.h",
        "drivers/soc/samsung/pwrcal8890/pwrcal-env.h",
        "drivers/soc/samsung/pwrcal8890/pwrcal-pll.c",
        "drivers/soc/samsung/pwrcal8890/pwrcal-pmu.c",
        "drivers/soc/samsung/pwrcal8890/pwrcal-pmu.h",
        "drivers/soc/samsung/pwrcal8890/pwrcal-rae.c",
        "drivers/soc/samsung/pwrcal8890/pwrcal-rae.h",
        "drivers/soc/samsung/pwrcal8890/pwrcal-vclk.c",
        "drivers/soc/samsung/pwrcal8890/pwrcal-vclk.h",
        "drivers/soc/samsung/pwrcal8890/pwrcal.h",
        "include/linux/soc/samsung/exynos8890-pwrcal.h",
    }
)
assert len(LEGACY_TARGETS) == 36


SOURCE_SUFFIXES = frozenset({".c", ".h"})
DATA_SUFFIXES = frozenset({".dts", ".dtsi", ".yaml", ".yml"})
DOC_SUFFIXES = frozenset({".md", ".rst", ".txt"})

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
LITERAL = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)

# Match declarations, enum members, types, constants and calls, not just a
# function-like token followed by '('.  The cal_gamma_* panel helpers are a
# separate subsystem, hence the explicit legacy CAL namespace below.
LEGACY_IDENTIFIER = re.compile(
    r"(?<![A-Za-z0-9_])(?:"
    r"[A-Za-z0-9_]*pwrcal[A-Za-z0-9_]*"
    r"|cal_(?:asv|clk|dfs|dram|pd|pm|rcc|vclk)[A-Za-z0-9_]*"
    r"|cal_(?:get_clk|get_vclk|init|rae_init|vtc_en)"
    r"|exynos8890_mif_cmu_[A-Za-z0-9_]+"
    r"|exynos8890_mif_cmu_request"
    r"|dvfs_rate_volt"
    r")(?![A-Za-z0-9_])",
    re.IGNORECASE,
)

# Prose may describe the removed design as PWRCAL.  These concrete build,
# include and source-path references are not historical prose and must vanish.
FORBIDDEN_REFERENCE = re.compile(
    r"(?:CONFIG_)?EXYNOS8890_PWRCAL\b"
    r"|(?:drivers/soc/samsung/)?pwrcal8890(?:/|\b)"
    r"|(?:include/linux/soc/samsung/)?exynos8890-pwrcal\.h\b"
    r"|samsung,exynos8890-pwrcal\b",
    re.IGNORECASE,
)

# git grep without a tree argument searches tracked files in the working tree.
# Use a deliberately broader prefilter than either final expression so this
# remains fast on a full kernel checkout without changing what is audited.
REFERENCE_CANDIDATE = r"pwrcal8890|exynos8890-pwrcal|EXYNOS8890_PWRCAL"
RUNTIME_CANDIDATE = (
    r"pwrcal|cal_(asv|clk|dfs|dram|pd|pm|rcc|vclk|get_clk|get_vclk|init|"
    r"rae_init|vtc_en)|exynos8890_mif_cmu_|dvfs_rate_volt"
)

DEFINE_NUMBER = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]+"
    r"\(?[ \t]*(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*[ \t]*\)?"
    r"(?:[ \t]|/\*|//|$)",
    re.MULTILINE,
)
ARRAY_INITIALIZER = re.compile(
    r"static\s+const\s+unsigned\s+long\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\]\s*"
    r"(?:__initconst\s*)?=\s*\{(?P<body>.*?)\n\s*\};",
    re.DOTALL,
)
CMU_INITIALIZER = re.compile(
    r"static\s+const\s+struct\s+samsung_cmu_info\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*"
    r"(?:__initconst\s*)?=\s*\{(?P<body>.*?)\n\s*\};",
    re.DOTALL,
)
CONTEXT_ARRAY = re.compile(r"\.clk_regs\s*=\s*([A-Za-z_][A-Za-z0-9_]*)")
CONTEXT_SIZE = re.compile(
    r"\.nr_clk_regs\s*=\s*ARRAY_SIZE\s*\(\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*\)"
)
SIMPLE_ENTRY = re.compile(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*|[A-Za-z_]\w*")


def numbered_offsets(offsets: list[int], description: str) -> dict[int, str]:
    return {offset: description for offset in offsets}


# These are register offsets, not macro spellings.  Resolving array entries to
# numbers means an alias or renamed #define cannot conceal a second context
# writer.  Read-only CCF clock descriptors may still refer to these fields;
# only .clk_regs save/restore arrays are forbidden from owning them.
TOP_DMC_OFFSETS = {
    **numbered_offsets(
        [0x0240, 0x0244, 0x0248, 0x024C, 0x0250, 0x0254],
        "CCORE matrix mux",
    ),
    **numbered_offsets(
        [0x03A0, 0x03A4, 0x03A8, 0x03AC, 0x03B0, 0x03B4],
        "CCORE matrix divider",
    ),
    0x0388: "MIF BUS switch mux",
    **numbered_offsets(
        [0x0540, 0x0544, 0x0548, 0x054C, 0x0550, 0x0554],
        "CCORE matrix mux status",
    ),
    0x0688: "MIF BUS switch mux status",
    0x0F10: "TOP0 root-clock control",
    0x0F1C: "TOP3 root-clock control",
    0x0F40: "TOP0 root-clock status",
    0x0F4C: "TOP3 root-clock status",
    **numbered_offsets(list(range(0x1000, 0x1038, 4)), "PSCDC control/FIFO bank"),
    **numbered_offsets(list(range(0x1080, 0x1094, 4)), "MIF clock control"),
}

CCORE_DMC_OFFSETS = {
    0x0200: "CCORE 800 user mux",
    0x0500: "CCORE 800 user mux status",
    0x1000: "CCORE PSCDC control",
}

MIF_DMC_OFFSETS = {
    0x0000: "MIF PLL lock",
    0x0100: "MIF PLL control 0",
    0x0104: "MIF PLL control 1",
    0x010C: "MIF PLL frequency detect",
    0x0200: "MIF PLL mux",
    0x0204: "MIF bus user mux",
    0x0208: "MIF ACLK mux",
    0x0210: "MIF PCLK mux",
    0x0214: "MIF HPM mux",
    0x0218: "MIF SMC PCLK mux",
    0x0400: "MIF PCLK divider",
    0x0404: "MIF SMC PCLK divider",
    0x0408: "MIF HPM divider",
    0x0600: "MIF PLL mux status",
    0x0604: "MIF bus user mux status",
    0x0608: "MIF ACLK mux status",
    0x0610: "MIF PCLK mux status",
    0x0614: "MIF HPM mux status",
    0x0618: "MIF SMC PCLK mux status",
    0x0A08: "DDRPHY automatic gate value",
    0x1000: "MIF PSCDC control",
    0x1A08: "DDRPHY manual gate control",
    0x1E08: "DDRPHY gate status",
    0x2000: "MIF LH_AXI_P QCH control",
}

CMU_DMC_OFFSETS = {
    "top_cmu_info": TOP_DMC_OFFSETS,
    "ccore_cmu_info": CCORE_DMC_OFFSETS,
    "mif0_cmu_info": MIF_DMC_OFFSETS,
    "mif1_cmu_info": MIF_DMC_OFFSETS,
    "mif2_cmu_info": MIF_DMC_OFFSETS,
    "mif3_cmu_info": MIF_DMC_OFFSETS,
}


def git(root: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(
        ("git", *args),
        cwd=root,
        check=check,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def repository_root() -> Path:
    result = subprocess.run(
        ("git", "rev-parse", "--show-toplevel"),
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return Path(result.stdout.strip())


def tracked_paths(root: Path) -> set[str]:
    output = git(root, "ls-files", "-z").stdout
    return {
        raw.decode("utf-8", errors="surrogateescape")
        for raw in output.split(b"\0")
        if raw
    }


def text_candidate_paths(root: Path, pattern: str) -> set[str]:
    result = git(root, "grep", "-IilE", pattern, "--", check=False)
    if result.returncode not in (0, 1):
        raise subprocess.CalledProcessError(
            result.returncode,
            result.args,
            output=result.stdout,
            stderr=result.stderr,
        )
    return {
        line.decode("utf-8", errors="surrogateescape")
        for line in result.stdout.splitlines()
        if line
    }


def relevant_text(path: str) -> bool:
    file = Path(path)
    name = file.name
    suffix = file.suffix.lower()

    if suffix in SOURCE_SUFFIXES or suffix in DATA_SUFFIXES:
        return True
    if name == "Makefile" or suffix == ".mk" or name.startswith("Kconfig"):
        return True
    if name == "defconfig" or name.endswith("_defconfig"):
        return True
    return path.startswith("Documentation/") and suffix in DOC_SUFFIXES


def runtime_scope(path: str) -> bool:
    """Limit the generic CAL namespace to Samsung/Exynos consumers."""

    lower = path.lower()
    return (
        "exynos8890" in lower
        or lower.startswith("drivers/clk/samsung/")
        or lower.startswith("drivers/pmdomain/samsung/")
        or lower.startswith("drivers/soc/samsung/")
        or lower.startswith("drivers/video/fbdev/exynos/")
        or lower.startswith("include/linux/soc/samsung/")
        or "/exynos/" in lower
    )


def blank(match: re.Match[str]) -> str:
    """Remove a token while retaining line numbers in diagnostics."""

    return re.sub(r"[^\n]", " ", match.group(0))


def line_for(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def source_line(text: str, line: int) -> str:
    lines = text.splitlines()
    if not lines or line > len(lines):
        return ""
    value = lines[line - 1].strip()
    if len(value) > 240:
        return value[:237] + "..."
    return value


def report_match(path: str, source: str, match: re.Match[str], kind: str) -> str:
    line = line_for(source, match.start())
    return f"{path}:{line}: {kind}: {source_line(source, line)}"


def audit_removed_targets(root: Path, tracked: set[str]) -> list[str]:
    violations: list[str] = []

    for path in sorted(LEGACY_TARGETS):
        if (root / path).exists():
            state = "tracked" if path in tracked else "present"
            violations.append(f"{path}: removed PWRCAL target is still {state}")

    prefix = "drivers/soc/samsung/pwrcal8890/"
    for path in sorted(tracked):
        if (
            path.startswith(prefix)
            and path not in LEGACY_TARGETS
            and (root / path).exists()
        ):
            violations.append(f"{path}: unexpected file in removed PWRCAL source tree")

    return violations


def audit_text_files(root: Path, tracked: set[str]) -> tuple[list[str], list[str]]:
    violations: list[str] = []
    read_errors: list[str] = []
    reference_paths = text_candidate_paths(root, REFERENCE_CANDIDATE) & tracked
    runtime_paths = {
        path
        for path in text_candidate_paths(root, RUNTIME_CANDIDATE) & tracked
        if runtime_scope(path)
    }

    for path in sorted(reference_paths | runtime_paths):
        file = root / path
        if path in LEGACY_TARGETS or not relevant_text(path) or not file.is_file():
            continue

        try:
            source = file.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            read_errors.append(f"{path}: unable to read tracked source: {error}")
            continue

        if path in reference_paths:
            for match in FORBIDDEN_REFERENCE.finditer(source):
                violations.append(
                    report_match(path, source, match, "forbidden PWRCAL reference")
                )

        if path not in runtime_paths or file.suffix.lower() not in SOURCE_SUFFIXES:
            continue

        code = COMMENT.sub(blank, source)
        code = LITERAL.sub(blank, code)
        for match in LEGACY_IDENTIFIER.finditer(code):
            violations.append(
                report_match(path, source, match, "legacy runtime identifier")
            )

    return violations, read_errors


def parse_initializers(
    source: str, pattern: re.Pattern[str]
) -> dict[str, tuple[str, int]]:
    return {
        match.group("name"): (match.group("body"), match.start("body"))
        for match in pattern.finditer(source)
    }


def parse_numeric_defines(source: str) -> dict[str, int]:
    return {
        match.group(1): int(match.group(2), 0)
        for match in DEFINE_NUMBER.finditer(source)
    }


def parse_context_entries(
    path: str,
    source: str,
    body: str,
    body_offset: int,
    defines: dict[str, int],
) -> tuple[list[tuple[str, int, int]], list[str]]:
    entries: list[tuple[str, int, int]] = []
    violations: list[str] = []
    clean = COMMENT.sub(blank, body)

    cursor = 0
    for item in clean.split(","):
        expression = item.strip()
        item_offset = body_offset + cursor + (len(item) - len(item.lstrip()))
        cursor += len(item) + 1
        if not expression:
            continue

        if not SIMPLE_ENTRY.fullmatch(expression):
            line = line_for(source, item_offset)
            violations.append(
                f"{path}:{line}: cannot prove CCF context ownership for "
                f"expression {expression!r}"
            )
            continue

        if re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]*", expression):
            value = int(expression.rstrip("uUlL"), 0)
        elif expression in defines:
            value = defines[expression]
        else:
            line = line_for(source, item_offset)
            violations.append(
                f"{path}:{line}: cannot resolve CCF context register "
                f"{expression!r}; ownership audit fails closed"
            )
            continue

        entries.append((expression, value, item_offset))

    return entries, violations


def audit_ccf_context(root: Path) -> list[str]:
    path = "drivers/clk/samsung/clk-exynos8890.c"
    file = root / path
    if not file.is_file():
        return [f"{path}: missing; cannot verify DMC/CCF context ownership"]

    source = file.read_text(encoding="utf-8", errors="replace")
    arrays = parse_initializers(source, ARRAY_INITIALIZER)
    infos = parse_initializers(source, CMU_INITIALIZER)
    defines = parse_numeric_defines(source)
    violations: list[str] = []

    for info_name, forbidden in CMU_DMC_OFFSETS.items():
        if info_name not in infos:
            violations.append(
                f"{path}: missing {info_name}; cannot verify DMC/CCF context ownership"
            )
            continue

        info_body, info_offset = infos[info_name]
        array_match = CONTEXT_ARRAY.search(info_body)
        size_match = CONTEXT_SIZE.search(info_body)

        if not array_match:
            if ".clk_regs" in info_body or ".nr_clk_regs" in info_body:
                line = line_for(source, info_offset)
                violations.append(
                    f"{path}:{line}: unsupported {info_name} context declaration; "
                    "ownership audit fails closed"
                )
            continue

        array_name = array_match.group(1)
        if not size_match or size_match.group(1) != array_name:
            line = line_for(source, info_offset + array_match.start())
            violations.append(
                f"{path}:{line}: {info_name} context size does not use "
                f"ARRAY_SIZE({array_name})"
            )

        if array_name not in arrays:
            line = line_for(source, info_offset + array_match.start())
            violations.append(
                f"{path}:{line}: context array {array_name!r} is not a "
                "simple static unsigned-long initializer; ownership audit fails closed"
            )
            continue

        body, body_offset = arrays[array_name]
        entries, parse_violations = parse_context_entries(
            path, source, body, body_offset, defines
        )
        violations.extend(parse_violations)

        for expression, offset, expression_offset in entries:
            if offset not in forbidden:
                continue
            line = line_for(source, expression_offset)
            violations.append(
                f"{path}:{line}: CCF context overlap: {info_name}/{array_name} "
                f"saves DMC-owned {forbidden[offset]} offset {offset:#06x} "
                f"through {expression}"
            )

    return violations


def audit() -> list[str]:
    root = repository_root()
    tracked = tracked_paths(root)
    violations = audit_removed_targets(root, tracked)
    text_violations, read_errors = audit_text_files(root, tracked)
    violations.extend(text_violations)
    violations.extend(read_errors)
    violations.extend(audit_ccf_context(root))
    return sorted(set(violations))


def main() -> int:
    try:
        violations = audit()
    except subprocess.CalledProcessError as error:
        stderr = error.stderr.decode("utf-8", errors="replace") if error.stderr else ""
        print(f"PWRCAL ownership audit could not inspect git files: {stderr}", file=sys.stderr)
        return 2

    if not violations:
        print(
            "Exynos8890 ownership audit passed: PWRCAL is absent and DMC-owned "
            "registers are excluded from CCF context restore."
        )
        return 0

    print(
        "Exynos8890 PWRCAL/CCF ownership audit failed "
        f"with {len(violations)} violation(s):",
        file=sys.stderr,
    )
    for violation in violations:
        print(f"  {violation}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
