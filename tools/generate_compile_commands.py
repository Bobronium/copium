from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import sysconfig


def discover_project_root_path() -> Path:
    return Path.cwd().resolve()


def discover_cpython_source_root_path() -> Path | None:
    raw_value = os.environ.get("CPYTHON_SRC")
    if not raw_value:
        return None

    candidate_path = Path(raw_value).expanduser().resolve()
    if not candidate_path.exists():
        print(f"CPython source directory {candidate_path!s} does not exist, ignoring.", flush=True)
        return None

    return candidate_path


def discover_python_include_directory(cpython_source_root_path: Path | None) -> Path:
    if cpython_source_root_path is not None:
        cpython_include_directory = cpython_source_root_path / "Include"
        if cpython_include_directory.exists():
            return cpython_include_directory.resolve()

    python_paths = sysconfig.get_paths()
    include_value = python_paths.get("include")
    if not include_value:
        raise RuntimeError("Python include directory not found in sysconfig.get_paths().")

    return Path(include_value).resolve()


def collect_c_source_files(root_directory_path: Path) -> list[Path]:
    if not root_directory_path.exists():
        return []
    return [source_file_path for source_file_path in root_directory_path.rglob("*.c") if source_file_path.is_file()]


def build_compile_command_entry(
    source_file_path: Path,
    project_root_path: Path,
    include_directories: list[Path],
) -> dict[str, str]:
    compiler_executable_value = sysconfig.get_config_var("CC") or "cc"
    compiler_executable_parts = shlex.split(compiler_executable_value)

    compiler_flags = ["-std=c11"]
    include_flags = [f"-I{directory_path}" for directory_path in include_directories]

    command_parts = (
        compiler_executable_parts
        + compiler_flags
        + include_flags
        + ["-c", str(source_file_path), "-o", os.devnull]
    )
    command_string = shlex.join(command_parts)

    return {
        "directory": str(project_root_path),
        "file": str(source_file_path),
        "command": command_string,
    }


def main() -> None:
    project_root_path = discover_project_root_path()
    cpython_source_root_path = discover_cpython_source_root_path()

    python_include_directory = discover_python_include_directory(cpython_source_root_path)
    python_internal_include_directory = python_include_directory / "internal"

    base_include_directories = [
        python_include_directory,
        python_internal_include_directory,
    ]

    compile_command_entries: list[dict[str, str]] = []

    project_source_directory_path = project_root_path / "src"
    project_source_files = collect_c_source_files(project_source_directory_path)
    for project_source_file_path in project_source_files:
        compile_command_entries.append(
            build_compile_command_entry(
                project_source_file_path,
                project_root_path,
                base_include_directories,
            )
        )

    if cpython_source_root_path is not None:
        cpython_include_directory = cpython_source_root_path / "Include"
        cpython_internal_include_directory = cpython_include_directory / "internal"
        cpython_include_directories = [
            cpython_include_directory,
            cpython_internal_include_directory,
        ]
        cpython_core_directories = [
            cpython_source_root_path / "Objects",
            cpython_source_root_path / "Python",
            cpython_source_root_path / "Modules",
            cpython_source_root_path / "Parser",
        ]
        for cpython_core_directory_path in cpython_core_directories:
            cpython_core_source_files = collect_c_source_files(cpython_core_directory_path)
            for cpython_source_file_path in cpython_core_source_files:
                compile_command_entries.append(
                    build_compile_command_entry(
                        cpython_source_file_path,
                        project_root_path,
                        cpython_include_directories,
                    )
                )

    output_path = project_root_path / "compile_commands.json"
    output_path.write_text(json.dumps(compile_command_entries, indent=2))
    print(f"Wrote {len(compile_command_entries)} compile commands to {output_path}", flush=True)


if __name__ == "__main__":
    main()