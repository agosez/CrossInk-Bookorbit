"""The scenarios, one module per functional area.

Every `scenario_<name>` function of an area's module is the scenario `<name>`: it takes
no argument and fails by raising AssertionError. Its docstring says what it proves.
"""

from __future__ import annotations

import importlib
from typing import Callable

AREAS = ("progress", "highlights", "bookmarks", "home", "catalog", "local_books", "book_update")


def collect() -> dict[str, tuple[str, Callable[[], None]]]:
    """Every scenario by name, with its area."""
    found: dict[str, tuple[str, Callable[[], None]]] = {}
    for area in AREAS:
        module = importlib.import_module(f"{__name__}.{area}")
        for name, function in vars(module).items():
            if name.startswith("scenario_") and getattr(function, "__module__", None) == module.__name__:
                scenario = name[len("scenario_"):]
                assert scenario not in found, f"scenario {scenario} is defined twice"
                found[scenario] = (area, function)
    return found
