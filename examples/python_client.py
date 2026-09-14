#!/usr/bin/env python3
"""Read-only example of using CSUDB from an external Python program."""

from __future__ import annotations

import argparse
from getpass import getpass
from pathlib import Path
import sys

# Keep this repository example runnable without installing a Python package.
# An external project can instead add the installed SDK directory to PYTHONPATH.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "sdk" / "python"))

import csudb  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="Query CSUDB through its Python driver")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6789)
    parser.add_argument("--user", default="root")
    parser.add_argument("--database", default="school")
    parser.add_argument("--min-age", type=int, default=20)
    args = parser.parse_args()

    password = getpass(f"CSUDB password for {args.user}: ")
    try:
        with csudb.connect(
            host=args.host,
            port=args.port,
            user=args.user,
            password=password,
            database=args.database,
        ) as database:
            print(f"Connected to {database.host}:{database.port}/{database.database}")

            # %s parameters are converted to SQL literals by the current SDK.
            cursor = database.execute(
                "SELECT id, name, age, major FROM student WHERE age >= %s;",
                (args.min_age,),
            )
            names = [column[0] for column in cursor.description or []]
            print("Columns:", ", ".join(names))
            for row in cursor.fetchall():
                print(" | ".join(row))
            print(f"Rows: {cursor.rowcount}")

            attributes = database.server_info()["attributes"]
            print(
                "Buffer Pool:",
                f"policy={attributes.get('replacement_policy', 'unknown')}",
                f"requests={attributes.get('requests', '0')}",
                f"hit_rate={attributes.get('hit_rate', '0%')}",
            )
    except csudb.Error as error:
        print(f"CSUDB error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
