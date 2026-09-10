"""JSON values for the fixed controller API; never pickle or import wire types."""

from __future__ import annotations

import dataclasses
import enum
import math
import types
from pathlib import Path
from typing import Any, Union, get_args, get_origin, get_type_hints


def encode(value):
    if dataclasses.is_dataclass(value):
        return {field.name: encode(getattr(value, field.name)) for field in dataclasses.fields(value)}
    if isinstance(value, enum.Enum):
        return value.value
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, dict):
        return {str(key): encode(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [encode(item) for item in value]
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


def restore(kind, value):
    """The caller supplies a local dataclass/type, not a class name from JSON."""
    origin, args = get_origin(kind), get_args(kind)
    if origin in (types.UnionType, Union):
        if value is None and type(None) in args:
            return None
        return restore(next(item for item in args if item is not type(None)), value)
    if origin in (list, tuple):
        values = [restore(args[0], item) for item in value] if args else value
        return tuple(values) if origin is tuple else values
    if origin is dict:
        return {key: restore(args[1], item) for key, item in value.items()}
    if dataclasses.is_dataclass(kind):
        hints = get_type_hints(kind)
        return kind(**{field.name: restore(hints[field.name], value[field.name])
                       for field in dataclasses.fields(kind) if field.name in value})
    if isinstance(kind, type) and issubclass(kind, enum.Enum):
        return kind(value)
    if kind is Path:
        return Path(value)
    if kind is float and value is None:
        return float("nan")
    return value
