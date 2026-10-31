#!/usr/bin/env python3
import json
import os
import subprocess
import sys


def main():
    try:
        raw_input = sys.stdin.read()
        if raw_input:
            data = json.loads(raw_input)
            tool_call = data.get("toolCall", {})
            args = tool_call.get("args", {})
            file_path = args.get("Path", "")

            if (
                file_path
                and file_path.endswith((".cpp", ".hpp", ".h", ".cc", ".cxx"))
                and os.path.isfile(file_path)
            ):
                subprocess.run(
                    ["/usr/bin/clang-format", "-i", file_path],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )
    except Exception:  # noqa: BLE001, S110
        pass
    finally:
        # Hooks require a valid JSON object on stdout
        sys.stdout.write("{}\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
