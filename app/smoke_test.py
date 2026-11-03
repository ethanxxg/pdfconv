"""Import smoke test -- run BEFORE packaging.

    python app/smoke_test.py

Imports every app module (and therefore every PyQt6 name they use) without
opening a window.  An ImportError shows up here in a second instead of after a
multi-minute PyInstaller build.
"""

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
for p in (HERE, ROOT):
    if p not in sys.path:
        sys.path.insert(0, p)

import worker      # noqa: E402,F401
import pagetools   # noqa: E402,F401
import main        # noqa: E402,F401

print("OK: worker / pagetools / main import cleanly")
