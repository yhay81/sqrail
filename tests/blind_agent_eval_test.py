#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import shutil
import subprocess
import tempfile
import types
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER_PATH = ROOT / "benchmarks" / "agent-eval" / "run.py"
SPEC = importlib.util.spec_from_file_location("sqrail_blind_agent_eval", RUNNER_PATH)
assert SPEC is not None and SPEC.loader is not None
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class BlindAgentEvaluationTest(unittest.TestCase):
    def test_schedule_crosses_contexts_and_keeps_pairs_distinct(self) -> None:
        schedule = RUNNER.make_schedule(
            agents=["codex"],
            conditions=["condition-a", "condition-b"],
            tasks=["schema_discovery"],
            contexts=["clean", "prior_error"],
            repetitions=2,
            seed=17,
        )
        self.assertEqual(len(schedule), 8)
        self.assertEqual({row["context"] for row in schedule}, {"clean", "prior_error"})
        self.assertEqual(len({row["run_id"] for row in schedule}), len(schedule))

    def test_real_world_contexts_are_explicit_and_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workspace = Path(directory)
            files = RUNNER.stage_context(
                workspace=workspace,
                context="noisy_workspace",
                token="fixed",
            )
            self.assertEqual(
                {path.name for path in files.values()},
                {
                    "unrelated_notes_fixed.md",
                    "unrelated_query_fixed.sql",
                    "unrelated_stale_fixed.csv",
                },
            )
            prompt = RUNNER.context_prompt("noisy_workspace", files)
            self.assertIn("unrelated retained artifacts", prompt)
            self.assertIn("do not substitute similarly typed files", prompt)
        recovery = RUNNER.context_prompt("prior_error", {})
        self.assertIn("different session", recovery)
        self.assertIn("start with `./rail --help`", recovery)

    def test_local_agent_uses_codex_ollama_without_remote_effort(self) -> None:
        arguments = types.SimpleNamespace(
            codex_model="gpt-5.6-luna",
            codex_effort="low",
            local_model="qwen3.5:4b",
            local_provider="ollama",
        )
        command = RUNNER.agent_command(
            agent="local",
            args=arguments,
            workspace=Path("/tmp/work"),
            prompt="do it",
            codex_bin=Path("/bin/codex"),
            claude_bin=Path("/bin/claude"),
            agy_bin=Path("/bin/agy"),
            agy_log_path=Path("/tmp/agy.log"),
        )
        self.assertIn("--oss", command)
        self.assertIn("ollama", command)
        self.assertIn("qwen3.5:4b", command)
        self.assertNotIn('model_reasoning_effort="low"', command)

    def test_agy_reported_model_exposes_tier_mismatch(self) -> None:
        events = [
            {
                "content": (
                    "The user changed setting `Model Selection` from None to "
                    "Gemini 3.5 Flash (Medium). No need to comment."
                )
            }
        ]
        reported = RUNNER.extract_reported_model(events, "gemini-3.5-flash-low")
        self.assertEqual(reported, "Gemini 3.5 Flash (Medium)")
        self.assertFalse(
            RUNNER.model_selection_matches("gemini-3.5-flash-low", reported)
        )
        self.assertTrue(
            RUNNER.model_selection_matches(
                "gpt-oss-120b-medium", "GPT-OSS 120B (Medium)"
            )
        )
        self.assertTrue(RUNNER.model_selection_matches("fable", "claude-fable-5"))

    def test_agy_command_uses_current_prompt_and_workspace_contract(self) -> None:
        arguments = types.SimpleNamespace(
            agy_model="gemini-3.6-flash-low",
            max_seconds=90,
        )
        command = RUNNER.agent_command(
            agent="gemini",
            args=arguments,
            workspace=Path("/tmp/isolated-workspace"),
            prompt="do it",
            codex_bin=Path("/bin/codex"),
            claude_bin=Path("/bin/claude"),
            agy_bin=Path("/bin/agy"),
            agy_log_path=Path("/tmp/agy.log"),
        )
        self.assertIn("--prompt", command)
        self.assertNotIn("--print", command)
        self.assertEqual(
            command[command.index("--add-dir") + 1],
            "/tmp/isolated-workspace",
        )
        prompt = command[command.index("--prompt") + 1]
        self.assertIn("Change to that directory", prompt)
        self.assertIn("/tmp/isolated-workspace", prompt)

    def test_provider_and_evaluation_failures_are_infrastructure(self) -> None:
        cases = (
            (
                1,
                '{"error":"authentication_failed"}',
                "OAuth session expired",
                "authentication",
            ),
            (
                1,
                "The 'gpt-5.4-nano' model is not supported when using Codex.",
                "",
                "model_unavailable",
            ),
            (
                1,
                "",
                "Our servers are experiencing high traffic right now.",
                "provider_overloaded",
            ),
            (
                70,
                "",
                "rail: cannot write private invocation log",
                "evaluation_log_unavailable",
            ),
        )
        for exit_code, transcript, stderr, expected in cases:
            with self.subTest(expected=expected):
                self.assertEqual(
                    RUNNER.infrastructure_failure(exit_code, transcript, stderr),
                    expected,
                )

    def test_sqrail_help_override_is_sanitized(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "help.txt"
            path.write_text(
                "sqrail uses DuckDB\n"
                "sqrail schema input.csv\n"
                "  sqrail run 'SELECT 1'\n",
                encoding="utf-8",
            )
            self.assertEqual(
                RUNNER.build_help(
                    "sqrail",
                    Path("/unused/sqrail"),
                    Path("/unused/duckdb"),
                    path,
                ),
                "rail uses rail\n./rail schema input.csv\n  ./rail run 'SELECT 1'\n",
            )

    def test_schema_columns_accepts_machine_readable_names(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schema.json"
            path.write_text(
                '[{"column_name":"event_id","data_type":"BIGINT"},'
                '{"name":"payload","column_type":"VARCHAR"}]\n',
                encoding="utf-8",
            )
            self.assertEqual(
                RUNNER.schema_columns(path),
                {"event_id": "BIGINT", "payload": "VARCHAR"},
            )

    def test_timeout_prompt_is_zsh_safe(self) -> None:
        names = {
            "fact_parquet": Path("fact.parquet"),
            "stdout": Path("partial.jsonl"),
            "diagnostic": Path("diagnostic.json"),
            "status": Path("status.txt"),
        }
        prompt = RUNNER.task_prompt("timeout_recovery", names)
        self.assertIn("write `$?` directly", prompt)
        self.assertIn("without assigning it to a shell variable", prompt)

    def test_join_prompt_defines_aggregation_grain(self) -> None:
        names = {
            "fact_parquet": Path("fact.parquet"),
            "dim_parquet": Path("dim.parquet"),
            "output": Path("answer.parquet"),
        }
        prompt = RUNNER.task_prompt("join_aggregate", names)
        self.assertIn("Group the joined rows by drug_class", prompt)
        self.assertIn("observation_count as count(*) for each class", prompt)

    def test_protocol_requires_help_as_first_rail_invocation(self) -> None:
        self.assertTrue(RUNNER.starts_with_help([["--help"], ["run", "SELECT 1"]]))
        self.assertFalse(RUNNER.starts_with_help([]))
        self.assertFalse(RUNNER.starts_with_help([["--version"]]))
        self.assertFalse(RUNNER.starts_with_help([["schema", "input.csv"]]))

    def test_launcher_records_concurrent_invocations_atomically(self) -> None:
        target = shutil.which("true")
        if target is None:
            self.skipTest("true executable is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            help_path = root / "help.txt"
            log_path = root / "invocations.log"
            launcher = root / "rail"
            help_path.write_text("rail help\n", encoding="utf-8")
            RUNNER.compile_launcher(
                arm="sqrail",
                launcher_source=ROOT / "benchmarks" / "agent-eval" / "launcher.c",
                target=Path(target),
                help_path=help_path,
                log_path=log_path,
                output=launcher,
            )
            processes = [
                subprocess.Popen(
                    [str(launcher), "run", str(index)],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                for index in range(64)
            ]
            self.assertTrue(all(process.wait() == 0 for process in processes))
            invocations = RUNNER.read_invocations(log_path)
            self.assertEqual(len(invocations), 64)
            self.assertEqual(
                {tuple(invocation) for invocation in invocations},
                {("run", str(index)) for index in range(64)},
            )

    def test_launcher_refuses_unwritable_audit_log(self) -> None:
        target = shutil.which("true")
        if target is None:
            self.skipTest("true executable is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            help_path = root / "help.txt"
            log_path = root / "log-directory"
            launcher = root / "rail"
            help_path.write_text("rail help\n", encoding="utf-8")
            log_path.mkdir()
            RUNNER.compile_launcher(
                arm="sqrail",
                launcher_source=ROOT / "benchmarks" / "agent-eval" / "launcher.c",
                target=Path(target),
                help_path=help_path,
                log_path=log_path,
                output=launcher,
            )
            result = subprocess.run(
                [str(launcher), "--help"],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 70)
            self.assertEqual(result.stdout, "")
            self.assertIn("cannot write private invocation log", result.stderr)

    def test_artifact_manifest_excludes_context_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            workspace = Path(directory)
            retained = workspace / "unrelated_notes.md"
            artifact = workspace / "answer.jsonl"
            retained.write_text("retain\n", encoding="utf-8")
            artifact.write_text('{"ok":true}\n', encoding="utf-8")
            manifest = RUNNER.artifact_manifest(
                workspace, excluded_names={retained.name}
            )
            self.assertEqual([row["name"] for row in manifest], [artifact.name])
            self.assertTrue(retained.is_file())


if __name__ == "__main__":
    unittest.main()
