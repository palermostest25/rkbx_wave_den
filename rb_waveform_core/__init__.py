# Copyright (c) mrmilbe

"""Rekordbox-style waveform core (production)."""

__version__ = '2.3.6'  # pyproject.toml reads this; a release tag v<this> builds it

from .config import WaveformColorConfig, WaveformRenderConfig, RenderMode
from .analysis import WaveformAnalysis
from .ANLZ import AnlzAnalysisResult, analyze_anlz_folder

__all__ = [
    "WaveformColorConfig",
    "WaveformRenderConfig",
    "RenderMode",
    "WaveformAnalysis",
    "AnlzAnalysisResult",
    "analyze_anlz_folder",
]
