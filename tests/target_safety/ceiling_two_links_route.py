# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_two_links_route.py
"""Fixed two-links receipt route boundary tests; existing capacity route helpers under the new NAME."""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Final
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ceiling_capacity_route

NAME: Final = 'task-5-prerequisites-ceiling-two-links.json'


def main() -> int:
    # Given: the exact approved two-links receipt name on the shared private fixtures.
    print(f'two links route wrapper; NAME={NAME}')
    with patch.object(ceiling_capacity_route, 'NAME', NAME):
        # When/Then: the five existing boundary tests run against the new route only;
        # the capacity suite keeps its own NAME and is retained unchanged.
        return ceiling_capacity_route.main()


if __name__ == '__main__':
    raise SystemExit(main())
